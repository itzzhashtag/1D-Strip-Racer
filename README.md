<div align="center">

# 🏎️ 1D RGB LED Strip Racing

**A Fast-Paced DIY Arduino Racing Game**

**by Aniket Chowdhury (aka `#Hashtag`)**

<img src="https://img.shields.io/badge/Status-Stable-brightgreen?style=for-the-badge&logo=arduino" />
<img src="https://img.shields.io/badge/Platform-Arduino%20%7C%20ESP32-blue?style=for-the-badge" />
<img src="https://img.shields.io/badge/Type-Interactive%20Game-orange?style=for-the-badge" />
<img src="https://img.shields.io/badge/Players-2--4-red?style=for-the-badge" />

</div>

---

## 🎬 Project Overview

**1D RGB LED Strip Racing** is a fun, competitive **button-mashing racing game** built using microcontrollers and addressable LEDs.

Each player controls a “car” represented by a glowing LED.
The faster you press your button, the faster your car moves.

> 🏁 First to complete all laps wins!

---

## ✨ Highlights

* 🏎️ Real-time **LED racing simulation**
* ⚡ Physics-based movement (**acceleration + friction**)
* 🎮 Supports **2 to 4 players**
* 🔢 Live lap counter using 7-segment display
* 🎵 Sound effects (countdown, lap, win, defeat)
* 🌈 Smooth animations (idle, countdown, win)
* 🔁 Multiple hardware versions:

  * Arduino Uno (basic)
  * Arduino Mega (4 players)
  * ESP32 (wireless control)

---

## ⚙️ How It Works

```text
Button Press
     ↓
Speed Increase (ACCEL)
     ↓
Friction Applied Over Time
     ↓
Distance Calculation
     ↓
LED Position Update
     ↓
Lap Detection → Display Update → Sound Feedback
```

---

## 🧠 Core Concept

> This is not just blinking LEDs — it's a **mini physics engine**.

Each player has:

* Speed
* Distance
* Lap count

The system simulates motion using simple physics:

* Acceleration when pressing
* Gradual slowdown when not

---

## 🔧 Hardware

### Basic Setup (Arduino Uno)

* Arduino Uno / compatible board
* WS2812B LED Strip
* TM1637 4-digit display
* Push buttons (×2–4)
* Passive buzzer
* External 5V power supply

---

## 🔌 Wiring Overview

| Component       | Arduino Pin |
| --------------- | ----------- |
| LED Strip Data  | D6          |
| Buzzer          | D8          |
| Player 1 Button | D2          |
| Player 2 Button | D3          |
| TM1637 CLK      | D10         |
| TM1637 DIO      | D11         |

⚠️ LED strip must use **external 5V power**

---

## 🎮 Game Flow

1. 🌈 Idle (rainbow animation)
2. ⏳ Countdown (3 → 2 → 1 → GO)
3. 🏎️ Racing (button mashing)
4. 🏆 Winner animation
5. 🔁 Back to idle

---

## 🧪 Variations

### 🧩 Arduino Mega (4 Players)

* More buttons
* More LEDs
* Multiplayer chaos 😄

### 📡 ESP32 Wireless Version

* Bluetooth / WiFi input
* Phone-based controls

### ⚡ Turbo Mode

* Higher acceleration
* More friction
* Faster gameplay

---

## 🎨 Visual System

| Effect       | Meaning   |
| ------------ | --------- |
| 🌈 Rainbow   | Idle      |
| 🔴🟠🟢 Sweep | Countdown |
| ⚪ Flash      | GO        |
| 🔴 / 🔵 Dots | Players   |
| 🟢 Flash     | Winner    |

---

## 🛠 Code Concepts

This project teaches:

* Variables & constants (`#define`)
* State machines (`enum`)
* Data structures (`struct`)
* Physics simulation
* Real-time loops

---

## 🚀 Future Improvements

* 📱 Mobile app control
* 🌐 Web dashboard (ESP32)
* 🤖 AI opponent
* 🧠 Smarter physics
* 🏆 Leaderboards

---

## 📸 Simulation

<img width="650" height="639" alt="image" src="https://github.com/user-attachments/assets/7d2418cc-b52e-4a82-a470-63cb51d7a33a" />

<img width="608" height="672" alt="image" src="https://github.com/user-attachments/assets/45b37150-da97-489d-8c1d-0b6434502876" />

---

## 📸 Wiring & Schematic

<img width="1108" height="881" alt="image" src="https://github.com/user-attachments/assets/540da65a-7080-4bd1-8a81-214ae0aeac00" />
<img width="1263" height="901" alt="image" src="https://github.com/user-attachments/assets/92b5516e-e39e-4106-977c-ae862197df55" />


---

## 👤 Author & Contact

👨 **Name:** Aniket Chowdhury (aka Hashtag)  
📧 **Email:** [micro.aniket@gmail.com](mailto:micro.aniket@gmail.com)  
💼 **LinkedIn:** [itzz-hashtag](https://www.linkedin.com/in/itzz-hashtag/)  
🐙 **GitHub:** [itzzhashtag](https://github.com/itzzhashtag)  
📸 **Instagram:** [@itzz_hashtag](https://instagram.com/itzz_hashtag)

---

## ⭐ Support

If you like this project:

* ⭐ Star the repo
* 🍴 Fork it
* 🔧 Build your own version

---

## 🔥 Final Thought

> This is not just a game —
> it’s a **hands-on introduction to physics, electronics, and fun.**

</div>
