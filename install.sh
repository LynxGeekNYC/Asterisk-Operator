#!/usr/bin/env bash
set -euo pipefail

APP_NAME="ami-callmon"
BIN_PATH="/usr/local/bin/${APP_NAME}"
CONF_DIR="/etc/ami-callmon"
CONF_FILE="${CONF_DIR}/config.env"
SYSTEMD_UNIT="/etc/systemd/system/${APP_NAME}.service"

ASTERISK_MANAGER="/etc/asterisk/manager.conf"
ASTERISK_EXTENSIONS="/etc/asterisk/extensions.conf"

AMI_USER="${AMI_USER:-callmon}"
AMI_SECRET="${AMI_SECRET:-}"
AMI_PERMIT_CIDR="${AMI_PERMIT_CIDR:-127.0.0.1/255.255.255.255}"

SUPERVISOR_CONTEXT="${SUPERVISOR_CONTEXT:-supervisor-monitor}"
SUPERVISOR_PIN="${SUPERVISOR_PIN:-1234}"          # CHANGE THIS
SUPERVISOR_PREFIX="${SUPERVISOR_PREFIX:-*55}"     # *55<target>
SUPERVISOR_ENDPOINT="${SUPERVISOR_ENDPOINT:-}"    # e.g. PJSIP/9000 (optional but required for in-app Monitor key)

ENABLE_SYSTEMD="${ENABLE_SYSTEMD:-1}"

CPP_SRC="${CPP_SRC:-./ami_callmon_tui.cpp}"

need_root() {
  if [[ "${EUID}" -ne 0 ]]; then
    echo "ERROR: Run as root (sudo)."
    exit 1
  fi
}

install_deps() {
  echo "[*] Installing dependencies"
  if command -v apt-get >/dev/null 2>&1; then
    apt-get update -y
    apt-get install -y --no-install-recommends \
      build-essential g++ make pkg-config \
      libboost-all-dev \
      libncurses5-dev libncursesw5-dev \
      ca-certificates
  elif command -v dnf >/dev/null 2>&1; then
    dnf install -y gcc-c++ make pkgconfig boost-devel ncurses-devel ca-certificates
  elif command -v yum >/dev/null 2>&1; then
    yum install -y gcc-c++ make pkgconfig boost-devel ncurses-devel ca-certificates
  else
    echo "ERROR: Supported package manager not found (apt/dnf/yum)."
    exit 1
  fi
}

backup_file() {
  local f="$1"
  if [[ -f "$f" ]]; then
    local ts
    ts="$(date +%Y%m%d_%H%M%S)"
    cp -a "$f" "${f}.bak.${ts}"
    echo "[*] Backup created: ${f}.bak.${ts}"
  else
    echo "ERROR: Required file not found: $f"
    exit 1
  fi
}

rand_secret() {
  tr -dc 'A-Za-z0-9!@#%^*()_+=-' </dev/urandom | head -c 32
  echo
}

ensure_ami_user() {
  echo "[*] Configuring AMI user in ${ASTERISK_MANAGER}"
  backup_file "${ASTERISK_MANAGER}"

  if [[ -z "${AMI_SECRET}" ]]; then
    AMI_SECRET="$(rand_secret)"
    echo "[*] Generated AMI secret."
  fi

  # Remove existing block for this user (from [user] to next [section])
  perl -0777 -i -pe "s/\\n\\[${AMI_USER}\\][\\s\\S]*?(?=\\n\\[[^\\]]+\\]|\\z)//g" "${ASTERISK_MANAGER}" || true

  cat >> "${ASTERISK_MANAGER}" <<EOF

[${AMI_USER}]
secret = ${AMI_SECRET}
read = system,call,log,verbose,command,agent,user,dtmf,reporting,cdr,dialplan
write = system,call,command,agent,user,dtmf,reporting,dialplan
permit = ${AMI_PERMIT_CIDR}
EOF

  echo "[*] AMI user [${AMI_USER}] configured."
}

ensure_supervisor_context() {
  echo "[*] Adding supervisor ChanSpy context to ${ASTERISK_EXTENSIONS}"
  backup_file "${ASTERISK_EXTENSIONS}"

  local begin="; BEGIN ${APP_NAME} ${SUPERVISOR_CONTEXT}"
  local end="; END ${APP_NAME} ${SUPERVISOR_CONTEXT}"

  if grep -qF "${begin}" "${ASTERISK_EXTENSIONS}"; then
    sed -i "/${begin}/,/${end}/d" "${ASTERISK_EXTENSIONS}"
  fi

  cat >> "${ASTERISK_EXTENSIONS}" <<EOF

${begin}
[${SUPERVISOR_CONTEXT}]
; Supervisor monitoring entry point
; Dial ${SUPERVISOR_PREFIX}<target> from an authorized supervisor device/context.
; Example target: PJSIP/1001-0000002a
exten => _${SUPERVISOR_PREFIX}.,1,NoOp(Supervisor monitor request: \${EXTEN})
 same => n,Authenticate(${SUPERVISOR_PIN})
 same => n,Set(TARGET=\${EXTEN:${#SUPERVISOR_PREFIX}})
 same => n,NoOp(Target: \${TARGET})
; q = quiet. Remove q for beeps if desired.
 same => n,ChanSpy(\${TARGET},q)
 same => n,Hangup()
${end}
EOF

  echo "[*] Supervisor context [${SUPERVISOR_CONTEXT}] installed."
}

reload_asterisk() {
  if command -v asterisk >/dev/null 2>&1; then
    echo "[*] Reloading Asterisk configuration"
    asterisk -rx "manager reload" || true
    asterisk -rx "dialplan reload" || true
  else
    echo "[!] asterisk CLI not found, reload manually if needed."
  fi
}

build_and_install() {
  echo "[*] Building ${APP_NAME} from ${CPP_SRC}"
  if [[ ! -f "${CPP_SRC}" ]]; then
    echo "ERROR: Source file not found: ${CPP_SRC}"
    exit 1
  fi

  local build_dir
  build_dir="$(mktemp -d)"
  cp -a "${CPP_SRC}" "${build_dir}/"
  pushd "${build_dir}" >/dev/null

  g++ -std=c++17 -O2 -pthread ami_callmon_tui.cpp -o "${APP_NAME}" -lboost_system -lncursesw
  install -m 0755 "${APP_NAME}" "${BIN_PATH}"

  popd >/dev/null
  rm -rf "${build_dir}"

  echo "[*] Installed binary to ${BIN_PATH}"
}

write_config() {
  echo "[*] Writing config to ${CONF_FILE}"
  mkdir -p "${CONF_DIR}"
  chmod 0750 "${CONF_DIR}"

  cat > "${CONF_FILE}" <<EOF
AMI_HOST=127.0.0.1
AMI_PORT=5038
AMI_USER=${AMI_USER}
AMI_SECRET=${AMI_SECRET}

# Optional: required if you want to press 'M' in the TUI to initiate supervisor monitoring
SUPERVISOR_ENDPOINT=${SUPERVISOR_ENDPOINT}
SUPERVISOR_CONTEXT=${SUPERVISOR_CONTEXT}
SUPERVISOR_PREFIX=${SUPERVISOR_PREFIX}
ORIGINATE_TIMEOUT_MS=20000
EOF

  chmod 0640 "${CONF_FILE}"
}

install_systemd() {
  if [[ "${ENABLE_SYSTEMD}" != "1" ]]; then
    echo "[*] Skipping systemd install."
    return
  fi

  echo "[*] Installing systemd unit: ${SYSTEMD_UNIT}"
  cat > "${SYSTEMD_UNIT}" <<EOF
[Unit]
Description=${APP_NAME} - Asterisk AMI Bridge/Call Monitor (TUI)
After=network.target asterisk.service
Wants=asterisk.service

[Service]
Type=simple
EnvironmentFile=${CONF_FILE}
ExecStart=${BIN_PATH} \${AMI_HOST} \${AMI_PORT} \${AMI_USER} \${AMI_SECRET}
Restart=on-failure
RestartSec=2

# Run as root because it reads system config and may be used in admin workflows.
User=root
Group=root

NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full
ProtectHome=true

[Install]
WantedBy=multi-user.target
EOF

  systemctl daemon-reload
  systemctl enable --now "${APP_NAME}.service" || true
  echo "[*] systemd installed. Use: systemctl status ${APP_NAME}.service"
}

summary() {
  echo
  echo "========================================"
  echo "Installed: ${APP_NAME}"
  echo "Binary:    ${BIN_PATH}"
  echo "Config:    ${CONF_FILE}"
  echo "AMI user:  ${AMI_USER}"
  echo "Permit:    ${AMI_PERMIT_CIDR}"
  echo "Supervisor context: [${SUPERVISOR_CONTEXT}]  prefix: ${SUPERVISOR_PREFIX}"
  echo "Supervisor endpoint for in-app monitor (optional): ${SUPERVISOR_ENDPOINT}"
  echo "========================================"
  echo
  echo "Run TUI manually:"
  echo "  ${BIN_PATH} 127.0.0.1 5038 ${AMI_USER} '${AMI_SECRET}'"
  echo
  echo "In TUI:"
  echo "  K = kick selected member from bridge"
  echo "  H = hangup selected member"
  echo "  B = destroy bridge"
  echo "  M = originate supervisor monitoring (requires SUPERVISOR_ENDPOINT)"
  echo
}

main() {
  need_root
  install_deps
  ensure_ami_user
  ensure_supervisor_context
  reload_asterisk
  build_and_install
  write_config
  install_systemd
  summary
}

main "$@"
