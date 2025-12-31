#!/usr/bin/env bash
set -euo pipefail

APP_NAME="ami-callmon"
BIN_PATH="/usr/local/bin/${APP_NAME}"
CONF_DIR="/etc/ami-callmon"
CONF_FILE="${CONF_DIR}/config.env"
SYSTEMD_UNIT="/etc/systemd/system/${APP_NAME}.service"

# FreePBX-safe AMI include
ASTERISK_MANAGER_CUSTOM="/etc/asterisk/manager_custom.conf"

AMI_USER="${AMI_USER:-callmon}"
AMI_SECRET="${AMI_SECRET:-}"
AMI_PERMIT_CIDR="${AMI_PERMIT_CIDR:-127.0.0.1/255.255.255.255}"

# Supervisor originate target for in-app Monitor key (optional)
SUPERVISOR_ENDPOINT="${SUPERVISOR_ENDPOINT:-}"       # e.g. PJSIP/9000
SUPERVISOR_CONTEXT="${SUPERVISOR_CONTEXT:-supervisor-monitor}"
SUPERVISOR_PREFIX="${SUPERVISOR_PREFIX:-*55}"
ENABLE_SYSTEMD="${ENABLE_SYSTEMD:-1}"

CPP_SRC="${CPP_SRC:-./ami_callmon_tui.cpp}"

need_root() {
  if [[ "${EUID}" -ne 0 ]]; then
    echo "ERROR: Run as root (sudo)."
    exit 1
  fi
}

detect_pkg_mgr() {
  if command -v apt-get >/dev/null 2>&1; then echo "apt"; return; fi
  if command -v dnf >/dev/null 2>&1; then echo "dnf"; return; fi
  if command -v yum >/dev/null 2>&1; then echo "yum"; return; fi
  echo "none"
}

install_deps() {
  local pm
  pm="$(detect_pkg_mgr)"
  echo "[*] Installing dependencies via: ${pm}"

  if [[ "${pm}" == "apt" ]]; then
    apt-get update -y
    apt-get install -y --no-install-recommends \
      build-essential g++ make pkg-config \
      libboost-all-dev \
      libncurses5-dev libncursesw5-dev \
      ca-certificates
  elif [[ "${pm}" == "dnf" ]]; then
    dnf install -y gcc-c++ make pkgconfig boost-devel ncurses-devel ca-certificates
  elif [[ "${pm}" == "yum" ]]; then
    yum install -y gcc-c++ make pkgconfig boost-devel ncurses-devel ca-certificates
  else
    echo "ERROR: Could not find apt-get, dnf, or yum. Install deps manually."
    exit 1
  fi
}

backup_file_if_exists() {
  local f="$1"
  if [[ -f "$f" ]]; then
    local ts
    ts="$(date +%Y%m%d_%H%M%S)"
    cp -a "$f" "${f}.bak.${ts}"
    echo "[*] Backup created: ${f}.bak.${ts}"
  fi
}

rand_secret() {
  tr -dc 'A-Za-z0-9!@#%^*()_+=-' </dev/urandom | head -c 32
  echo
}

ensure_manager_custom_exists() {
  if [[ ! -f "${ASTERISK_MANAGER_CUSTOM}" ]]; then
    echo "[*] Creating ${ASTERISK_MANAGER_CUSTOM}"
    touch "${ASTERISK_MANAGER_CUSTOM}"
    chmod 0640 "${ASTERISK_MANAGER_CUSTOM}" || true
  fi
}

ensure_ami_user_freepbx() {
  echo "[*] Configuring AMI user in ${ASTERISK_MANAGER_CUSTOM}"
  ensure_manager_custom_exists
  backup_file_if_exists "${ASTERISK_MANAGER_CUSTOM}"

  if [[ -z "${AMI_SECRET}" ]]; then
    AMI_SECRET="$(rand_secret)"
    echo "[*] Generated AMI secret."
  fi

  # Remove existing block for this user from manager_custom.conf
  perl -0777 -i -pe "s/\\n\\[${AMI_USER}\\][\\s\\S]*?(?=\\n\\[[^\\]]+\\]|\\z)//g" "${ASTERISK_MANAGER_CUSTOM}" || true

  cat >> "${ASTERISK_MANAGER_CUSTOM}" <<EOF

[${AMI_USER}]
secret = ${AMI_SECRET}
read = system,call,log,verbose,command,agent,user,dtmf,reporting,cdr,dialplan
write = system,call,command,agent,user,dtmf,reporting,dialplan
permit = ${AMI_PERMIT_CIDR}
EOF

  echo "[*] AMI user [${AMI_USER}] written to manager_custom.conf"
}

reload_freepbx() {
  if command -v fwconsole >/dev/null 2>&1; then
    echo "[*] Reloading FreePBX and Asterisk via fwconsole"
    fwconsole reload || true
  elif command -v amportal >/dev/null 2>&1; then
    echo "[*] Reloading FreePBX via amportal"
    amportal a r || true
  elif command -v asterisk >/dev/null 2>&1; then
    echo "[!] fwconsole not found, reloading Asterisk directly"
    asterisk -rx "manager reload" || true
  else
    echo "[!] Could not reload automatically. Please run: fwconsole reload"
  fi
}

build_and_install_binary() {
  echo "[*] Building ${APP_NAME} from ${CPP_SRC}"
  if [[ ! -f "${CPP_SRC}" ]]; then
    echo "ERROR: Source file not found: ${CPP_SRC}"
    echo "Place ami_callmon_tui.cpp next to this script, or set CPP_SRC=/path/to/ami_callmon_tui.cpp"
    exit 1
  fi

  local build_dir
  build_dir="$(mktemp -d)"
  cp -a "${CPP_SRC}" "${build_dir}/ami_callmon_tui.cpp"
  pushd "${build_dir}" >/dev/null

  g++ -std=c++17 -O2 -pthread ami_callmon_tui.cpp -o "${APP_NAME}" -lboost_system -lncursesw
  install -m 0755 "${APP_NAME}" "${BIN_PATH}"

  popd >/dev/null
  rm -rf "${build_dir}"

  echo "[*] Installed binary: ${BIN_PATH}"
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

# Optional: required only if you want to press 'M' in the TUI to originate supervisor monitoring.
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
Description=${APP_NAME} - FreePBX Asterisk AMI Bridge Monitor (TUI)
After=network.target asterisk.service
Wants=asterisk.service

[Service]
Type=simple
EnvironmentFile=${CONF_FILE}
ExecStart=${BIN_PATH} \${AMI_HOST} \${AMI_PORT} \${AMI_USER} \${AMI_SECRET}
Restart=on-failure
RestartSec=2
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
  echo "[*] systemd enabled. Check: systemctl status ${APP_NAME}.service"
}

summary() {
  echo
  echo "========================================"
  echo "FreePBX install complete (no dialplan injected)"
  echo "Binary:   ${BIN_PATH}"
  echo "Config:   ${CONF_FILE}"
  echo "AMI user: ${AMI_USER}"
  echo "Permit:   ${AMI_PERMIT_CIDR}"
  echo "========================================"
  echo
  echo "Next step: manually add the supervisor context (copy/paste file provided) and connect it in FreePBX."
  echo "Then reload: fwconsole reload"
  echo
}

main() {
  need_root
  install_deps
  ensure_ami_user_freepbx
  reload_freepbx
  build_and_install_binary
  write_config
  install_systemd
  summary
}

main "$@"
