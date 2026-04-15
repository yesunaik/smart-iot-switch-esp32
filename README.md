# 🔌 VerifAI Smart IoT Switch (ESP32 + Backend)

## 🚀 Overview
VerifAI Smart IoT Switch is a complete end-to-end IoT solution that enables **remote control of electrical devices** using ESP32, a backend server, and a web interface.

This system combines **embedded firmware, backend APIs, and a web-based UI** to deliver a real-world automation solution.

---

## 🔥 Key Features
- 🔹 Remote ON/OFF control of electrical devices
- 🔹 Real-time device status monitoring
- 🔹 WiFi-based communication between ESP32 and server
- 🔹 Web-based configuration and control interface
- 🔹 ESP32-hosted UI using LittleFS/SPIFFS
- 🔹 Backend API for control and monitoring

---

## 🧠 System Architecture


User → Web UI → Backend Server → ESP32 → Relay Control


---

## 📁 Project Structure


smart-iot-switch-esp32/
│
├── esp32/
│ ├── mac_add.ino
│ └── data/
│ ├── index.html
│ ├── config.html
│ ├── success.html
│ └── verifaiLogo.jpg
│
├── backend/
│ ├── main.py
│ └── index.html
│
└── README.md


---

## ⚙️ Setup Instructions

### 1️⃣ ESP32 Firmware Setup
- Open `mac_add.ino` in Arduino IDE
- Select your ESP32 board
- Connect ESP32 and upload the code

---

### 2️⃣ Upload Web Files (LittleFS / SPIFFS)
- Install LittleFS uploader plugin
- Upload files inside `/esp32/data` folder to ESP32

---

### 3️⃣ Backend Setup
```bash
pip install flask  # or required dependencies
python main.py
4️⃣ Run the System
Connect ESP32 to WiFi
Access the web interface
Control devices remotely
🛠️ Tech Stack

Embedded:

ESP32 (Arduino)

Backend:

Python (Flask / FastAPI)

Frontend:

HTML, CSS, JavaScript

Communication:

WiFi (HTTP-based control)
👨‍💻 My Contribution
Designed complete IoT system architecture
Developed ESP32 firmware for device control
Built backend server for API communication
Created web-based UI for user interaction
Integrated hardware and software components
📈 Impact
Enabled remote control of electrical devices
Reduced manual switching effort
Built a production-ready IoT automation system
Demonstrated real-time device communication
🚀 Future Improvements
Mobile app integration
Cloud deployment (AWS / Azure)
Voice assistant integration
Real-time analytics dashboard
🔗 Author

Yesu Naik
📧 yesunaik2001@gmail.com

🔗 GitHub: https://github.com/yesunaik

🔗 LinkedIn: https://linkedin.com/in/yesu-naik-749734246


---
