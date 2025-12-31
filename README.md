# Asterisk-Operator
Operator script to Manage and drop calls in Asterisk. Additional Functionality to auto-drop calls where there is silence.

````markdown
# Asterisk AMI Call and Bridge Monitor (Asterisk 20, PJSIP, ncurses TUI)

This project provides a real-time, event-driven monitoring and control tool for Asterisk 20 using AMI. It displays active calls grouped by bridges, shows call duration and direction, and allows bridge and channel control operations.

It also supports supervisor monitoring (listen, whisper, barge) via Asterisk `ChanSpy()` by originating a call to an authorized supervisor endpoint. The application does not process RTP audio.

## Features

- Live terminal UI (ncurses)
- Event-driven state model (no polling UI loop for call state)
- Calls grouped by `BridgeUniqueid`
- Shows per-call duration (from first `BridgeEnter`)
- Shows bridge participants (channels)
- Best-effort call direction classification: inbound, outbound, internal, unknown
- Control actions:
  - Hang up a selected channel
  - Kick a selected channel from a bridge (drop a caller)
  - Destroy a bridge (terminate the call at the bridge level)
  - Supervisor monitor initiation by AMI Originate into `supervisor-monitor` context

## High-Level Architecture

### Data Flow

```text
+-------------------+         AMI TCP/5038          +-------------------------+
| ami-callmon (TUI) | <---------------------------> | Asterisk 20 (PJSIP)     |
| C++17 + ncurses   |                               | AMI Events + Actions    |
+-------------------+                               +-------------------------+
         |                                                     |
         | AMI Events                                           | Dialplan
         v                                                     v
+-------------------+                               +-------------------------+
| In-memory model   |                               | supervisor-monitor       |
| bridges, channels |                               | ChanSpy() variants        |
+-------------------+                               +-------------------------+
         |
         v
+-------------------+
| Live TUI display  |
| selection, filter |
| actions           |
+-------------------+
````

### Supervisor Monitoring Flow (no RTP in app)

```text
Operator presses M on a selected channel
  |
  v
ami-callmon sends AMI Originate:
  Channel: PJSIP/<SUPERVISOR_EXT>
  Context: supervisor-monitor
  Exten:   *55<PJSIP/channel-unique>
  |
  v
Supervisor answers phone
  |
  v
Dialplan runs ChanSpy(<target>, mode)
  |
  v
Supervisor listens, whispers, or barges using Asterisk-native audio path
```

## Repository Layout

Suggested layout:

```text
.
├── ami_callmon_tui.cpp
├── install_callmon_asterisk.sh
├── install_callmon_freepbx_nodialplan.sh
├── freepbx_supervisor_monitor_context.conf
└── README.md
```

## Prerequisites

* Asterisk 20
* PJSIP enabled and in use
* AMI enabled and reachable
* Build dependencies:

  * g++
  * Boost (libboost_system)
  * ncursesw

## Build (manual)

Debian/Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y build-essential g++ libboost-all-dev libncursesw5-dev
g++ -std=c++17 -O2 -pthread ami_callmon_tui.cpp -o ami-callmon -lboost_system -lncursesw
```

Run:

```bash
./ami-callmon 127.0.0.1 5038 <ami_user> '<ami_secret>'
```

## Installation

There are two installation paths:

* Standalone Asterisk installation, installer can inject AMI user and dialplan context directly.
* FreePBX installation, installer must not assume dialplan routing. You will manually paste the context and wire it via the GUI.

### A) Standalone Asterisk Installation

1. Place `ami_callmon_tui.cpp` and `install_callmon_asterisk.sh` on the server.
2. Run the installer:

```bash
chmod +x install_callmon_asterisk.sh
sudo SUPERVISOR_PIN=778899 SUPERVISOR_ENDPOINT="PJSIP/9000" ./install_callmon_asterisk.sh
```

3. Start the service:

```bash
sudo systemctl status ami-callmon.service --no-pager
```

### B) FreePBX Installation (correct, manual dialplan integration)

FreePBX generates and overwrites core dialplan files. Use the FreePBX-safe installer, then manually add and route the monitoring context.

#### Step 1. Install the binary and AMI user (no dialplan injection)

1. Place `ami_callmon_tui.cpp` and `install_callmon_freepbx_nodialplan.sh` on the FreePBX server.
2. Run:

```bash
chmod +x install_callmon_freepbx_nodialplan.sh
sudo SUPERVISOR_ENDPOINT="PJSIP/9000" ./install_callmon_freepbx_nodialplan.sh
```

This writes the AMI user to:

* `/etc/asterisk/manager_custom.conf`

and installs:

* `/usr/local/bin/ami-callmon`
* `/etc/ami-callmon/config.env`
* `ami-callmon.service` (optional, enabled by default)

#### Step 2. Paste the supervisor monitoring context (manual)

Use the provided file:

* `freepbx_supervisor_monitor_context.conf`

Copy and paste its contents into:

* `/etc/asterisk/extensions_custom.conf`

Then reload:

```bash
fwconsole reload
```

Confirm the context is present:

```bash
asterisk -rx "dialplan show supervisor-monitor"
```

#### Step 3. Wire access in FreePBX GUI (required)

Adding the context is not enough. You must make it reachable from supervisor extensions only.

Below is a GUI walkthrough with two common methods.

## FreePBX GUI Walkthrough (step-by-step)

FreePBX versions vary by deployment. Menu names are consistent across FreePBX 15, 16, and 17, but some screens differ. Use the closest match.

### Method 1 (recommended): Custom Destinations + Misc Applications

Goal:

* Create a safe “entry point” number supervisors dial, then prompt for the target channel string.
* Practical use is usually to monitor agents by extension patterns, queue channels, or selected channel names surfaced by the TUI.

Because ChanSpy targets channel names, most operators use the TUI "M" key to originate monitoring automatically. The GUI wiring primarily ensures the context is reachable and restricted.

#### 1. Create a Custom Destination

1. FreePBX Admin UI
2. Go to **Admin** → **Custom Destinations**
3. Click **Add Custom Destination**
4. Set:

   * **Custom Destination**: `supervisor-monitor,*55,1`
   * **Description**: `Supervisor Monitor (ChanSpy)`
5. Click **Submit**
6. Click **Apply Config**

Result:

* FreePBX can now route to `supervisor-monitor,*55,1`

#### 2. Create a Misc Application for supervisors

1. Go to **Applications** → **Misc Applications**
2. Click **Add Misc Application**
3. Set:

   * **Description**: `Supervisor Monitor`
   * **Feature Code**: choose an internal code, for example `9550`
   * **Destination**: select your Custom Destination `Supervisor Monitor (ChanSpy)`
4. Click **Submit**
5. Click **Apply Config**

Result:

* Dialing `9550` will route into `supervisor-monitor,*55,1`

Important limitation:

* Dialing `9550` alone does not include a channel target. This is why the TUI originates to `*55<target>` directly.
* The GUI method is still useful as a controlled “door” into the context if you later extend the dialplan to prompt for a target (advanced) or for static targets.

#### 3. Restrict access to supervisors only

Pick one of the following depending on your environment:

* If you use **Class of Service** (commercial modules), restrict dialing of `9550` to supervisor COS.
* If you do not have COS, enforce restrictions with:

  * extension permissions at the endpoint level, or
  * a PIN in the dialplan via `Authenticate()` (already present in the context file)

Recommended:

* Keep `Authenticate(<PIN>)` enabled and set a strong PIN.
* Restrict the code `9550` via COS if you have it.

### Method 2: Feature Code style access (if your deployment supports it cleanly)

Some deployments prefer a feature-code-like prefix, for example `*55`, but FreePBX Feature Codes are not always designed to pass dynamic suffixes.

If you want supervisors to dial `*551<channel>` manually, you usually do it outside of the FreePBX Feature Code module, by allowing from-internal-custom to route to your context. This is an advanced pattern and depends on your FreePBX dialplan includes.

If you proceed, do it intentionally:

1. Ensure your context exists in `extensions_custom.conf`
2. Ensure it is included from an internal context supervisors can dial from (commonly `from-internal-custom`)
3. Use endpoint and PIN controls to ensure only supervisors can reach it

Because this is version and design dependent, Method 1 plus the TUI originate approach is the operationally reliable option.

## Using the TUI

### Keys

* Up/Down: select a call (bridge)
* Tab: cycle through bridge members (channels)
* F: cycle direction filter (all, inbound, outbound, internal)
* H: hang up selected member channel
* K: kick selected member from the bridge
* B: destroy selected bridge
* M: originate supervisor monitoring for selected member (requires `SUPERVISOR_ENDPOINT`)
* L: show audit log
* Q: quit

### Configure supervisor originate

Edit `/etc/ami-callmon/config.env`:

```ini
SUPERVISOR_ENDPOINT=PJSIP/9000
SUPERVISOR_CONTEXT=supervisor-monitor
SUPERVISOR_PREFIX=*55
```

Restart:

```bash
systemctl restart ami-callmon.service
```

## Asterisk vs FreePBX Installation Differences

### Standalone Asterisk

* You control Asterisk configuration directly.
* The installer can safely write to:

  * `/etc/asterisk/manager.conf`
  * `/etc/asterisk/extensions.conf`
* The monitoring context becomes reachable as soon as you reload the dialplan.

### FreePBX

* FreePBX generates and overwrites Asterisk configuration.
* You must use FreePBX-safe include files:

  * `/etc/asterisk/manager_custom.conf`
  * `/etc/asterisk/extensions_custom.conf`
* Adding a context is not enough. You must explicitly wire it into call routing using the FreePBX GUI or a controlled include strategy.
* This is why the FreePBX installer does not automatically “activate” the monitoring context.

## Security Notes

* Restrict AMI to localhost whenever possible.
* Use a strong AMI secret.
* Restrict `permit=` to a specific host or subnet.
* Restrict access to the supervisor monitoring context:

  * Use `Authenticate()` with a strong PIN
  * Restrict routing to supervisor extensions only
* Log and audit supervisor monitoring usage according to your policy and jurisdiction.

## Troubleshooting

### AMI login fails

* Check `/etc/asterisk/manager_custom.conf` or `/etc/asterisk/manager.conf`
* Confirm Asterisk is listening on 5038:

  * `ss -lntp | grep 5038`
* Reload:

  * Standalone: `asterisk -rx "manager reload"`
  * FreePBX: `fwconsole reload`

### No bridges show up

* Not all call legs are bridged at all times.
* Check that `BridgeCreate` and `BridgeEnter` events are enabled.
* Confirm in CLI:

  * `asterisk -rx "core show channels"`
  * `asterisk -rx "bridge show all"`

### Monitor key M does nothing

* Ensure `SUPERVISOR_ENDPOINT` is set.
* Confirm the supervisor endpoint can be dialed.
* Confirm the dialplan context exists:

  * `asterisk -rx "dialplan show supervisor-monitor"`
* Confirm your PIN is correct.

## License

Add your preferred license. Common choices: MIT, Apache-2.0, GPLv3.

## Disclaimer

This tool is for authorized administration and supervision. Ensure you comply with all applicable laws, consent requirements, and organizational policies for call monitoring and recording.

```

