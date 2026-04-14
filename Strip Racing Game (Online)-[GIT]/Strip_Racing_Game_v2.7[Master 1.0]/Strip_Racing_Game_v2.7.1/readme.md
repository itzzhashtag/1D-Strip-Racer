<div align="center">

# 🏎️ LED Strip Racing — Arduino Uno Edition

### Version v2.7.1  

<img src="https://img.shields.io/badge/Board-Arduino%20Uno-blue?style=for-the-badge" />
<img src="https://img.shields.io/badge/Players-2-green?style=for-the-badge" />
<img src="https://img.shields.io/badge/LEDs-WS2812B-orange?style=for-the-badge" />

</div>

---

## 🎬 Overview

This is the **stable Arduino Uno version** of the LED Racing Game.

Designed for:

* 🧑‍🎓 Students
* 🧑‍🏫 Classroom teaching
* 🛠 Beginner makers

---

## ✨ Features

* 🏎️ 2-player racing
* 🔢 TM1637 lap display
* 🎵 Sound effects (buzzer)
* 🌈 Idle rainbow animation
* ⏳ Smooth countdown system
* 🏁 Lap-based win detection
* 💤 Auto timeout (inactivity reset)
* 🎬 Advanced win animation (fade + sync sound)

---

## ⚙️ Key Settings

```cpp
#define NUM_LEDS        120
#define TOTAL_LAPS      2
#define ACCEL           0.20f
#define FRICTION        0.015f
#define TICK_MS         5
```

---

## 🔧 Hardware Required

* Arduino Uno
* WS2812B LED Strip (60–120 LEDs)
* TM1637 Display
* 2 Push Buttons
* Passive Buzzer
* 5V External Power Supply

---

## 🔌 Pin Configuration

| Function    | Pin |
| ----------- | --- |
| LED Data    | 6   |
| Buzzer      | 8   |
| Player 1    | 2   |
| Player 2    | 3   |
| Display CLK | 10  |
| Display DIO | 11  |

---

## 🎮 How to Play

1. Power ON → Rainbow idle
2. Press any button → Start
3. Watch countdown
4. Tap fast to move
5. Complete laps
6. First wins 🎉

---

## 🧠 Internal Logic

### Game States

```cpp
STATE_IDLE
STATE_COUNTDOWN
STATE_RACING
STATE_WIN
```

### Player System

Each player has:

* Speed
* Distance
* Lap count

---

## 📸 Simulation

<img width="650" height="639" alt="image" src="https://github.com/user-attachments/assets/7d2418cc-b52e-4a82-a470-63cb51d7a33a" />

<img width="608" height="672" alt="image" src="https://github.com/user-attachments/assets/45b37150-da97-489d-8c1d-0b6434502876" />

---

## 📸 Wiring & Schematic

<img width="1108" height="881" alt="image" src="https://github.com/user-attachments/assets/540da65a-7080-4bd1-8a81-214ae0aeac00" />
<img width="1263" height="901" alt="image" src="https://github.com/user-attachments/assets/92b5516e-e39e-4106-977c-ae862197df55" />


---
## 🎬 What’s New in v2.7.x

* 🎵 Non-blocking win jingle + display sync
* 🌈 Smooth fade-in to idle (no abrupt jump)
* 🎨 Improved animations
* ⚙️ Better timing accuracy
* 🔧 Cleaner structure

---

## ⚠️ Important Notes

* ❗ Do NOT power LEDs from Arduino
* 🔗 Common GND required
* ⚡ Use 330Ω resistor on data line
* 💡 Keep wires short

---

## 🧪 Simulation

👉 https://wokwi.com/projects/461126131423519745

---

## 🔮 Upgrade Paths

* Add more players → Arduino Mega
* Go wireless → ESP32 version
* Add obstacles / game modes

---

## 👤 Author & Contact

👨 **Name:** Aniket Chowdhury (aka Hashtag)  
📧 **Email:** [micro.aniket@gmail.com](mailto:micro.aniket@gmail.com)  
💼 **LinkedIn:** [itzz-hashtag](https://www.linkedin.com/in/itzz-hashtag/)  
🐙 **GitHub:** [itzzhashtag](https://github.com/itzzhashtag)  
📸 **Instagram:** [@itzz_hashtag](https://instagram.com/itzz_hashtag)

---

## 🏁 Final Note

> This version is the **foundation** —
> simple, stable, and perfect for learning.

</div>
