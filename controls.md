## Wii input config mapping

| Wiimote | config id |
| --- | :-: |
| dpad up | `0` |
| dpad down | `1` |
| dpad left | `2` |
| dpad right | `3` |
| A | `4` |
| B | `5` |
| 1 | `6` |
| 2 | `7` |
| plus | `8` |
| minus | `9` |
| home | `10` |

| Nunchuk | config id |
| --- | :-: |
| Z | `100` |
| C | `101` |
| joystick up | `900` |
| joystick down | `901` |
| joystick left | `902` |
| joystick right | `903` |

| Classic controller | config id |
| --- | :-: |
| dpad up | `200` |
| dpad down | `201` |
| dpad left | `202` |
| dpad right | `203` |
| A | `204` |
| B | `205` |
| X | `206` |
| Y | `207` |
| Z left | `208` |
| Z right | `209` |
| L full | `210` |
| R full | `211` |
| plus | `212` |
| minus | `213` |
| home | `214` |
| L joystick up | `910` |
| L joystick down | `911` |
| L joystick left | `912` |
| L joystick right | `913` |
| R joystick up | `920` |
| R joystick down | `921` |
| R joystick left | `922` |
| R joystick right | `923` |

| Guitar Hero 3 | config id |
| --- | :-: |
| yellow | `300` |
| green | `301` |
| blue | `302` |
| red | `303` |
| orange | `304` |
| plus | `305` |
| minus | `306` |
| strum up | `307` |
| strum down | `308` |

## Debug overlay (F3) on the Wii

Hold **MINUS (−)** and press **PLUS (+)** on the Wiimote to show/hide the F3 overlay
(Classic Controller: **Y**, unchanged; PC: **F3**). The chord deliberately uses the two
hotbar-scroll buttons: the stray scroll-left from pressing − and scroll-right from
pressing + cancel each other, so your selected slot ends where it started. Configured as
key code `9008` (`hold*1000 + press`) in `input.debug_overlay` — any binding entry
`>= 1000` is a two-button chord.
