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
SUPERVISOR_PIN="${SUPERVISOR_PIN:-1234}"          # Change this
SUPERVISOR_PREFIX="${SUPERVISOR_PREFIX:-*55}"     # Dial *55<target> to monitor
ENABLE_SYSTEMD="${ENABLE_SYSTEMD:-1}"

CPP_SRC="${CPP_SRC:-./ami_callmon.cpp}"

need_root() {
  if [[ "${EUID}" -ne 0 ]]; then
    echo "ERROR: Run as root (sudo)."
    exit 1
  fi
}

detect_os() {
  if [[ -f /etc/os-release ]]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    echo "${ID:-unknown}"
  else
    echo "unknown"
  fi
}

install_deps() {
  local os
  os="$(detect_os)"

  echo "[*] Installing dependencies for OS: ${os}"

  if command -v apt-get >/dev/null 2>&1; then
    apt-get update -y
    apt-get install -y --no-install-recommends \
      build-essential g++ make pkg-config \
      libboost-all-dev \
      libncurses5-dev libncursesw5-dev \
      git ca-certificates
  elif command -v dnf >/dev/null 2>&1; then
    dnf install -y \
      gcc-c++ make pkgconfig \
      boost-devel ncurses-devel \
      git ca-certificates
  elif command -v yum >/dev/null 2>&1; then
    yum install -y \
      gcc-c++ make pkgconfig \
      boost-devel ncurses-devel \
      git ca-certificates
  else
    echo "ERROR: Supported package manager not found (apt/dnf/yum). Install deps manually."
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
  # 32 chars, URL-safe-ish
  tr -dc 'A-Za-z0-9!@#%^*()_+=-' </dev/urandom | head -c 32
  echo
}

ensure_ami_user() {
  echo "[*] Ensuring AMI user exists in ${ASTERISK_MANAGER}"

  backup_file "${ASTERISK_MANAGER}"

  if [[ -z "${AMI_SECRET}" ]]; then
    AMI_SECRET="$(rand_secret)"
    echo "[*] Generated AMI secret (stored in ${CONF_FILE} after install)."
  fi

  # Remove any existing block for this user (basic idempotency)
  # This deletes from [user] header until the next [section] header.
  sed -i -E "/^\[${AMI_USER}\]$/,/^\[[^]]+\]$/ { /^\[${AMI_USER}\]$/b; /^\[[^]]+\]$/b; d; }" "${ASTERISK_MANAGER}" || true
  # If header exists with no content, remove it too
  sed -i -E "/^\[${AMI_USER}\]$/,/^\[[^]]+\]$/ { /^\[${AMI_USER}\]$/d; }" "${ASTERISK_MANAGER}" || true

  # Append fresh block
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
  echo "[*] Ensuring supervisor monitoring context exists in ${ASTERISK_EXTENSIONS}"
  backup_file "${ASTERISK_EXTENSIONS}"

  local begin_marker="; BEGIN ${APP_NAME} ${SUPERVISOR_CONTEXT}"
  local end_marker="; END ${APP_NAME} ${SUPERVISOR_CONTEXT}"

  # Remove existing managed block
  if grep -qF "${begin_marker}" "${ASTERISK_EXTENSIONS}"; then
    sed -i "/${begin_marker}/,/${end_marker}/d" "${ASTERISK_EXTENSIONS}"
  fi

  cat >> "${ASTERISK_EXTENSIONS}" <<EOF

${begin_marker}
[${SUPERVISOR_CONTEXT}]
; Supervisor monitoring entry point.
; Dial ${SUPERVISOR_PREFIX}<target> from a SUPERVISOR device/context.
; Examples:
;   ${SUPERVISOR_PREFIX}PJSIP/1001-0000002a  (exact channel)
;   ${SUPERVISOR_PREFIX}PJSIP/1001           (pattern, if it matches active channels)
;
exten => _${SUPERVISOR_PREFIX}.,1,NoOp(Supervisor monitor request: \${EXTEN})
 same => n,Authenticate(${SUPERVISOR_PIN})
 same => n,Set(TARGET=\${EXTEN:${#SUPERVISOR_PREFIX}})
 same => n,NoOp(Target: \${TARGET})
; q = quiet. Remove 'q' if you want audible beeps.
; For whisper/barge variants, create additional extensions with options 'w' or 'b'.
 same => n,ChanSpy(\${TARGET},q)
 same => n,Hangup()
${end_marker}
EOF

  echo "[*] Dialplan context [${SUPERVISOR_CONTEXT}] appended."
}

reload_asterisk() {
  if command -v asterisk >/dev/null 2>&1; then
    echo "[*] Reloading Asterisk manager and dialplan"
    asterisk -rx "manager reload" || true
    asterisk -rx "dialplan reload" || true
  else
    echo "[!] asterisk CLI not found. Please reload Asterisk manually."
  fi
}

build_and_install_binary() {
  echo "[*] Building ${APP_NAME} from ${CPP_SRC}"

  if [[ ! -f "${CPP_SRC}" ]]; then
    echo "ERROR: Source file not found: ${CPP_SRC}"
    echo "Place ami_callmon.cpp next to this script, or set CPP_SRC=/path/to/file."
    exit 1
  fi

  local build_dir
  build_dir="$(mktemp -d)"
  cp -a "${CPP_SRC}" "${build_dir}/"
  pushd "${build_dir}" >/dev/null

  # If you later switch to ncurses TUI, you may add -lncursesw here.
  g++ -std=c++17 -O2 -pthread ami_callmon.cpp -o "${APP_NAME}"

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
# ${APP_NAME} configuration (sourced by systemd unit)
AMI_HOST=127.0.0.1
AMI_PORT=5038
AMI_USER=${AMI_USER}
AMI_SECRET=${AMI_SECRET}

# Optional: supervisor originate settings (if your C++ tool implements it later)
SUPERVISOR_CONTEXT=${SUPERVISOR_CONTEXT}
SUPERVISOR_PREFIX=${SUPERVISOR_PREFIX}
EOF

  chmod 0640 "${CONF_FILE}"
}

install_systemd() {
  if [[ "${ENABLE_SYSTEMD}" != "1" ]]; then
    echo "[*] Skipping systemd service install (ENABLE_SYSTEMD!=1)."
    return 0
  fi

  echo "[*] Installing systemd service: ${SYSTEMD_UNIT}"

  cat > "${SYSTEMD_UNIT}" <<EOF
[Unit]
Description=${APP_NAME} - Asterisk AMI Call Control Monitor
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

# Hardening (adjust if needed)
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full
ProtectHome=true

[Install]
WantedBy=multi-user.target
EOF

  systemctl daemon-reload
  systemctl enable --now "${APP_NAME}.service" || {
    echo "[!] Could not start service automatically. You can start it with:"
    echo "    systemctl start ${APP_NAME}.service"
  }

  echo "[*] systemd service installed and enabled."
}

print_summary() {
  echo
  echo "========================================"
  echo "Install complete"
  echo "Binary:      ${BIN_PATH}"
  echo "Config:      ${CONF_FILE}"
  echo "AMI user:    ${AMI_USER}"
  echo "AMI permit:  ${AMI_PERMIT_CIDR}"
  echo "Supervisor:  context=[${SUPERVISOR_CONTEXT}] prefix=[${SUPERVISOR_PREFIX}] PIN=[${SUPERVISOR_PIN}]"
  echo "========================================"
  echo
  echo "Supervisor monitoring usage (from a supervisor phone/context):"
  echo "  Dial ${SUPERVISOR_PREFIX}<target>"
  echo "  Example: ${SUPERVISOR_PREFIX}PJSIP/1001-0000002a"
  echo
  echo "Service status:"
  echo "  systemctl status ${APP_NAME}.service --no-pager"
  echo
  echo "If you changed manager.conf/extensions.conf outside this installer, reload Asterisk:"
  echo "  asterisk -rx \"manager reload\""
  echo "  asterisk -rx \"dialplan reload\""
  echo
}

main() {
  need_root
  install_deps
  ensure_ami_user
  ensure_supervisor_context
  reload_asterisk
  build_and_install_binary
  write_config
  install_systemd
  print_summary
}

main "$@"
