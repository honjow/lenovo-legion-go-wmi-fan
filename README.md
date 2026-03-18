# lenovo-legion-wmi-fan

A DKMS kernel driver that exposes fan curve control for the **Lenovo Legion Go**
family of handheld gaming PCs through the standard Linux `hwmon` interface.

## Why is this needed?

The Legion Go uses the GameZone WMI GUID (`887B54E3-DDDC-4B2C-8B88-68A26A8835D0`)
for all firmware interactions.  The mainline kernel driver `lenovo_wmi_gamezone`
already binds to that GUID, but it only uses method IDs 43/44/45 for
`platform_profile` (quiet / balanced / performance switching).

Fan curve control lives in the same GUID under completely different method IDs
(5/6/16/35/36) that the mainline driver never touches.  The mainline
`lenovo_wmi_other` driver could in theory provide `hwmon` fan nodes, but it
requires a firmware capability GUID (`B642801B-…`) that Legion Go hardware
does not expose, so no fan nodes appear.

This driver fills that gap without touching or conflicting with any mainline code.

## Supported devices

| Model                  | DMI product version   |
|------------------------|-----------------------|
| Lenovo Legion Go       | `Legion Go 8APU1`     |
| Lenovo Legion Go S     | `Legion Go S 8APU1`   |
| Lenovo Legion Go S     | `Legion Go S 8ARP1`   |
| Lenovo Legion Go 2     | `Legion Go 8ASP2`     |
| Lenovo Legion Go 2     | `Legion Go 8AHP2`     |

## How it coexists with mainline drivers

This driver is implemented as a **platform driver** that calls the WMI firmware
methods directly via `wmi_evaluate_method()` (the global GUID-based API).  It
does **not** bind to the WMI device instance, which means it coexists safely
with `lenovo_wmi_gamezone` even though both reference the same GameZone GUID.

On Legion Go hardware, the GameZone GUID has a single device instance that
`lenovo_wmi_gamezone` binds to for `platform_profile` support.  A second
`wmi_driver` would never get probed because there is no second instance.
Using the GUID-based call API sidesteps that limitation entirely.

Each driver uses completely different method IDs and neither interferes with
the other.

## Prerequisites

- Linux kernel **6.8** or later (required for the modern `hwmon` API used here)
- `dkms` package
- `linux-headers` matching your running kernel

```
# Arch / Manjaro / SteamOS
sudo pacman -S dkms linux-headers

# Ubuntu / Debian / Nobara
sudo apt install dkms linux-headers-$(uname -r)
```

## Installation

### DKMS (recommended — survives kernel upgrades)

```bash
git clone https://github.com/honjow/lenovo-legion-go-wmi-fan.git
cd lenovo-legion-go-wmi-fan
sudo bash install.sh
```

### Manual (single kernel version)

```bash
git clone https://github.com/honjow/lenovo-legion-go-wmi-fan.git
cd lenovo-legion-go-wmi-fan
make
sudo make install
sudo modprobe lenovo-legion-wmi-fan
```

## Uninstall

```bash
sudo bash uninstall.sh   # DKMS
# or
sudo make uninstall      # manual
```

## Usage

After loading the module, a new hwmon device appears (e.g. `/sys/class/hwmon/hwmonN`
where `name` reads `legion_wmi_fan`).  A `fan_fullspeed` attribute is also
created directly under the WMI device in `/sys/bus/wmi/devices/`.

### Identify the hwmon device

```bash
for h in /sys/class/hwmon/hwmon*; do
    echo "$h: $(cat $h/name)";
done
```

### Reading the current fan speed

```bash
cat /sys/class/hwmon/hwmonN/fan1_input
```

The raw value returned by the firmware is a percentage (0–100); it is exposed
as-is.

### Reading the current fan curve

```bash
for i in $(seq 1 10); do
    temp=$(cat /sys/class/hwmon/hwmonN/pwm1_auto_point${i}_temp)
    pwm=$(cat  /sys/class/hwmon/hwmonN/pwm1_auto_point${i}_pwm)
    echo "Point $i: $((temp/1000))°C → PWM $pwm ($(( pwm*100/255 ))%)"
done
```

### Setting a custom fan curve

First put the fan in manual mode, then write the desired curve:

```bash
# Enable manual (custom curve) mode
echo 1 | sudo tee /sys/class/hwmon/hwmonN/pwm1_enable

# Temperatures in millidegrees Celsius, PWM values 0-255
sudo tee /sys/class/hwmon/hwmonN/pwm1_auto_point1_temp  <<< 30000   # 30°C
sudo tee /sys/class/hwmon/hwmonN/pwm1_auto_point1_pwm   <<< 0
sudo tee /sys/class/hwmon/hwmonN/pwm1_auto_point2_temp  <<< 40000   # 40°C
sudo tee /sys/class/hwmon/hwmonN/pwm1_auto_point2_pwm   <<< 51      # ~20%
sudo tee /sys/class/hwmon/hwmonN/pwm1_auto_point3_temp  <<< 50000
sudo tee /sys/class/hwmon/hwmonN/pwm1_auto_point3_pwm   <<< 102     # ~40%
# … and so on up to point 10
```

### Returning to automatic firmware control

```bash
echo 2 | sudo tee /sys/class/hwmon/hwmonN/pwm1_enable
```

### Enabling full-speed mode

```bash
# Via pwm1_enable
echo 0 | sudo tee /sys/class/hwmon/hwmonN/pwm1_enable

# Or via the dedicated sysfs attribute on the platform device
echo 1 | sudo tee /sys/bus/platform/devices/legion-wmi-fan/fan_fullspeed
```

### Using with `fancontrol` (lm-sensors)

`fancontrol` can read the hwmon attributes directly.  Run `pwmconfig` to
generate a `/etc/fancontrol` configuration file that references the
`legion_wmi_fan` hwmon device.

## `pwm1_enable` values

| Value | Meaning                                      |
|-------|----------------------------------------------|
| `0`   | Full speed (fans always at 100 %)            |
| `1`   | Manual — custom curve from `auto_point*` applies |
| `2`   | Automatic — firmware EC controls the fans    |

## Troubleshooting

**Module doesn't load / no hwmon node appears**

Check `dmesg | grep legion_wmi`:

- `"Not a supported Legion Go device"` — your device's DMI product version
  string is not in the match table.  Open an issue with the output of
  `sudo dmidecode -s system-product-version`.
- `"GetFanCount failed"` — the firmware method IDs don't match your BIOS
  version.  Capture `sudo acpidump > acpidump.txt` and open an issue.

**Kernel version too old**

```bash
uname -r   # must be 6.14 or later
```

**Check loaded modules**

```bash
lsmod | grep lenovo
# lenovo_wmi_gamezone and lenovo_wmi_fan can both appear — that is expected
```

**Force-load for testing**

```bash
sudo modprobe lenovo-legion-wmi-fan
dmesg | tail -20
```

## License

GPL-2.0 — see [LICENSE](LICENSE)
