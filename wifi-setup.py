#!/usr/bin/python3 -I
"""Put an HP LaserJet P1102w on Wi-Fi from macOS, over its USB cable.

The printer exposes its Embedded Web Server (EWS) on a vendor USB interface
("HP EWS", class ff/02/10). This script drives the printer's own wireless
settings form through that interface using build/ews-usb, so no Windows
installer, WPS button or Wireless Direct hotspot is needed.

  ./wifi-setup.py status          show radio, network, IP and firmware
  ./wifi-setup.py scan            list networks the printer can see
  ./wifi-setup.py join [--ssid N] turn the radio on and join a network
                      [--dry-run] show the request (password redacted)

The Wi-Fi password is read with no echo (Terminal) or from a hidden macOS
dialog (no terminal). It's piped to ews-usb on stdin and never written to
disk or passed on a command line.
"""
import argparse
import os
import re
import subprocess
import sys
import time
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
EWS = os.path.join(HERE, "build", "ews-usb")
FORM = "/cgi-bin/ip_wireless_result.htm"
FORM_HEADERS = ["Content-Type: application/x-www-form-urlencoded",
                "Referer: http://localhost/SSI/Auth/ip_wireless.htm"]


def die(msg):
    print("error: " + msg, file=sys.stderr)
    sys.exit(1)


def ews(path, method="GET", body=None, headers=()):
    """Run one HTTP request over USB. Returns (status, body_text)."""
    if not os.access(EWS, os.X_OK):
        die("build/ews-usb missing. Run: brew install libusb && make wifi")
    cmd = [EWS, "-v", "-X", method]
    for h in headers:
        cmd += ["-H", h]
    if body is not None:
        cmd += ["-d", "@-"]
    cmd.append(path)
    p = subprocess.run(cmd, input=(body or "").encode(), capture_output=True)
    head = p.stderr.decode("latin-1")
    m = re.search(r"HTTP/1\.\d (\d{3})", head)
    if not m:
        die("no response from the printer over USB.\n  " + head.strip() + "\n"
            "  Is the USB cable connected and the printer on? If requests time out,\n"
            "  run 'build/ews-usb -R /IoMgmt/Adapters' (the printer may restart itself,\n"
            "  about 30 s, keeping its settings) or turn the printer off and on.")
    return int(m.group(1)), p.stdout.decode("utf-8", "replace")


def tag(xml, name, default=""):
    m = re.search(r"<[\w]*:?%s>([^<]*)<" % re.escape(name), xml)
    return m.group(1) if m else default


def unhex(s):
    try:
        return bytes.fromhex(s).decode("utf-8")
    except ValueError:
        return s


def wifi0():
    code, xml = ews("/IoMgmt/Adapters")
    if code != 200:
        die("printer returned HTTP %d for /IoMgmt/Adapters" % code)
    for blk in re.findall(r"<io:Adapter>(.*?)</io:Adapter>", xml, re.S):
        if tag(blk, "Name") == "wifi0":
            return {"power": tag(blk, "Power"), "ssid": unhex(tag(blk, "SSID")),
                    "connected": tag(blk, "IsConnected") == "true",
                    "status": tag(blk, "NetworkStatus")}
    die("printer has no wifi0 adapter (is this a P1102w?)")


def ipv4():
    _, xml = ews("/DevMgmt/IOConfigDyn.xml")
    m = re.search(r"<dd:IPVersionSupport>ipv4</dd:IPVersionSupport>(.*?)</dd3:IPVersionConfig>", xml, re.S)
    s = m.group(1) if m else ""
    return {"ip": tag(s, "IPAddress", "0.0.0.0"), "gateway": tag(s, "DefaultGateway"),
            "hostname": tag(xml, "CurrentHostname")}


def scan():
    for _ in range(10):
        code, xml = ews("/IoMgmt/Adapters/wifi0/WifiNetworks")
        if code != 202:           # 202 = scan still running
            break
        time.sleep(3)
    if code != 200:
        die("scan failed (HTTP %d). Is the radio on? Try: ./wifi-setup.py join" % code)
    nets = {}
    for blk in re.findall(r"<io:WifiNetwork>(.*?)</io:WifiNetwork>", xml, re.S):
        ssid = unhex(tag(blk, "SSID"))
        if not ssid:
            continue
        dbm = int(tag(blk, "dBm", "-200") or -200)
        if ssid not in nets or dbm > nets[ssid]["dbm"]:
            nets[ssid] = {"ssid": ssid, "dbm": dbm, "channel": tag(blk, "Channel"),
                          "security": tag(blk, "EncryptionType")}
    return sorted(nets.values(), key=lambda n: -n["dbm"])


def radio_on():
    code, page = ews(FORM, "POST", "ID+207=1&ID+208=1&ID+207=0&Apply=Apply", FORM_HEADERS)
    if code != 200 or "successfully" not in page:
        die("printer did not accept the radio-on request (HTTP %d)" % code)
    for _ in range(10):
        time.sleep(2)
        if wifi0()["power"] == "on":
            return
    die("radio did not turn on")


def ask_password(ssid):
    if sys.stdin.isatty():
        import getpass
        return getpass.getpass("Wi-Fi password for %s: " % ssid)
    script = ('text returned of (display dialog "Enter the Wi-Fi password for \\"%s\\"." '
              'default answer "" with hidden answer with title "HP LaserJet P1102w Wi-Fi Setup" '
              'buttons {"Cancel", "Join Network"} default button "Join Network" giving up after 300)'
              % ssid.replace("\\", "\\\\").replace('"', '\\"'))
    p = subprocess.run(["osascript", "-e", script], capture_output=True)
    if p.returncode != 0:
        die("password dialog cancelled")
    return p.stdout.decode("utf-8").rstrip("\n")


def cmd_status(_):
    w, ip = wifi0(), ipv4()
    _, prod = ews("/DevMgmt/ProductConfigDyn.xml")
    print("Printer:      %s (firmware %s)" % (tag(prod, "MakeAndModel"), tag(prod, "Date", "?")))
    print("Wi-Fi radio:  %s" % w["power"])
    print("Network:      %s (%s)" % (w["ssid"] or "-", "connected" if w["connected"] else w["status"]))
    if ip["ip"] != "0.0.0.0":
        print("IPv4:         %s  gateway %s" % (ip["ip"], ip["gateway"]))
        print("Hostname:     %s.local" % ip["hostname"])
        print("AirPrint:     add it in System Settings > Printers & Scanners")


def cmd_scan(_):
    if wifi0()["power"] != "on":
        die("Wi-Fi radio is off. './wifi-setup.py join' turns it on.")
    for n in scan():
        print("%-32s ch %-3s %-6s %4d dBm" % (n["ssid"], n["channel"], n["security"], n["dbm"]))


def cmd_join(a):
    if wifi0()["power"] != "on":
        print("Turning the printer's Wi-Fi radio on...")
        radio_on()
    print("Scanning (2.4 GHz only)...")
    nets = scan()
    if a.ssid:
        net = next((n for n in nets if n["ssid"] == a.ssid), None)
        if not net:
            die("the printer can't see '%s'. It only supports 2.4 GHz." % a.ssid)
    else:
        if not nets:
            die("no networks found")
        for i, n in enumerate(nets, 1):
            print("  %d) %-32s %4d dBm  %s" % (i, n["ssid"], n["dbm"], n["security"]))
        if not sys.stdin.isatty():
            die("no terminal to pick from; pass --ssid NAME")
        pick = input("Join which network? [1] ").strip() or "1"
        if not pick.isdigit() or not 1 <= int(pick) <= len(nets):
            die("invalid choice")
        net = nets[int(pick) - 1]

    sec = net["security"].lower()
    if sec in ("none", "open", ""):
        mode, password = "0", ""
    elif "wep" in sec:
        die("WEP networks aren't supported by this tool")
    else:
        mode = "2"                                  # WPA/WPA2 personal
        password = "x" * 8 if a.dry_run else ask_password(net["ssid"])
        if not 8 <= len(password) <= 63 or any(not 32 <= ord(c) <= 126 for c in password):
            die("the printer needs an 8-63 character printable-ASCII passphrase")
    if len(net["ssid"].encode()) > 32:
        die("SSID longer than 32 bytes")

    # Exactly what the printer's own wireless page submits.
    fields = [("ID 207", "1"), ("ID 208", "1"), ("ID 209", "2"), ("ID 216", net["ssid"]),
              ("ID 217", mode), ("ID 219", password), ("ID 222", "0"), ("ID 223", "1"),
              ("ID 224.0", ""), ("ID 224.1", ""), ("ID 224.2", ""), ("ID 224.3", ""),
              ("ID 218", "0"), ("ID 207", "0"), ("Apply", "Apply")]
    body = urllib.parse.urlencode(fields)
    if a.dry_run:
        print("POST %s\n%s" % (FORM, body.replace(urllib.parse.quote_plus(password), "[redacted]")
                               if password else body))
        return
    code, page = ews(FORM, "POST", body, FORM_HEADERS)
    password = body = None
    if code != 200 or "successfully" not in page:
        die("printer rejected the settings (HTTP %d)" % code)

    print("Joining %s..." % net["ssid"])
    for _ in range(30):
        time.sleep(3)
        w, ip = wifi0(), ipv4()
        if w["connected"] and ip["ip"] != "0.0.0.0":
            print("Connected: %s at %s (%s.local)" % (w["ssid"], ip["ip"], ip["hostname"]))
            print("Add it on any Mac in System Settings > Printers & Scanners (AirPrint).")
            return
    die("not connected after 90 s. Check the password, then run './wifi-setup.py status'.")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("status")
    sub.add_parser("scan")
    j = sub.add_parser("join")
    j.add_argument("--ssid")
    j.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    {"status": cmd_status, "scan": cmd_scan, "join": cmd_join, None: cmd_status}[a.cmd](a)


if __name__ == "__main__":
    main()
