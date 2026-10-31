#!/usr/bin/env python3
"""Remote Wii power control -- SwitchBot Bot over LOCAL Bluetooth (no hub).

The Bot is mounted on the Wii's POWER button (press mode). The button is
handled at the hardware level, so a press works even when the software is
fully wedged (e.g. after watchdog_inject). One press toggles on<->off.

A Shelly Plug (Gen4, local RPC over HTTP) sits on the Wii's mains as the
doomsday layer: cutting mains recovers from states even the power button
cannot. After mains restore the Wii wakes in STANDBY, so hard_cycle ends
with a Bot press to actually turn it on. The plug's initial_state is
pinned to "on" so a Shelly reboot can never strand the outlet dead.

Usage:
    python tools/wii_power_ble.py press           # single toggle press (Bot)
    python tools/wii_power_ble.py cycle [gap_s]   # press, wait, press (off->on)
    python tools/wii_power_ble.py shelly_on|shelly_off|shelly_status
    python tools/wii_power_ble.py hard_cycle      # mains off -> on -> Bot press

BLE protocol: official OpenWonderLabs/SwitchBotAPI-BLE spec.
"""
import asyncio
import json
import os
import sys
import urllib.request

from bleak import BleakClient, BleakScanner

SHELLY_IP = os.environ.get("SHELLY_IP", "192.0.2.70")  # shellyplugusg4-acebe6f77e3c (DHCP; rediscover if it moves)

BOT_MAC = "EC:2E:C1:06:27:2B"  # SwitchBot "Bot 2B" (cloud deviceId EC2EC106272B)
PRESS_CHAR = "cba20002-224d-11e6-9fb8-0002a5d5c51b"
CMD_PRESS = bytes([0x57, 0x01, 0x00])


async def find_bot():
    dev = await BleakScanner.find_device_by_address(BOT_MAC, timeout=15.0)
    if dev is None:
        devs = await BleakScanner.discover(timeout=10.0)
        cands = [d for d in devs if d.name and "WoHand" in d.name]
        if not cands:
            raise SystemExit("FAIL: Bot not found over BLE (in range? battery?)")
        dev = cands[0]
    return dev


async def press_once():
    dev = await find_bot()
    print(f"found: {dev.name} {dev.address}")
    async with BleakClient(dev, timeout=20.0) as client:
        await client.write_gatt_char(PRESS_CHAR, CMD_PRESS, response=True)
    print("PRESS sent")


def shelly(rpc):
    with urllib.request.urlopen(f"http://{SHELLY_IP}/rpc/{rpc}", timeout=6) as r:
        return json.load(r)


async def main():
    action = sys.argv[1] if len(sys.argv) > 1 else "press"
    if action == "press":
        await press_once()
    elif action == "shelly_on":
        print(shelly("Switch.Set?id=0&on=true"), "-> outlet on")
    elif action == "shelly_off":
        print(shelly("Switch.Set?id=0&on=false"), "-> outlet off")
    elif action == "shelly_status":
        st = shelly("Switch.GetStatus?id=0")
        # This console's measured draws: ~13.5W running, ~7.6W standby
        # (WiiConnect24 standby keeps the radio warm -- NOT <1W like plain off).
        w = st["apower"]
        guess = "RUNNING" if w > 11 else ("STANDBY" if w > 3 else "no console / mains cut")
        print(f"outlet={'ON' if st['output'] else 'OFF'} power={w}W voltage={st['voltage']}V -> {guess}")
    elif action == "hard_cycle":
        print(shelly("Switch.Set?id=0&on=false"), "-> mains CUT")
        await asyncio.sleep(10)
        print(shelly("Switch.Set?id=0&on=true"), "-> mains restored (Wii in standby)")
        await asyncio.sleep(5)
        await press_once()
        print("hard_cycle complete -- Wii should be booting")
    elif action == "cycle":
        gap = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0
        await press_once()
        print(f"waiting {gap:.0f}s ...")
        await asyncio.sleep(gap)
        await press_once()
    else:
        raise SystemExit(f"unknown action: {action}")


if __name__ == "__main__":
    asyncio.run(main())
