# Multi-Device Mesh & Coordination Plan

## 1. Overview
Since you have 3 devices, we can transform them into a localized tactical mesh network. Instead of just independent controllers, they will communicate with each other to share status, coordinate "attacks" (relay triggers), or create a redundant security perimeter.

## 2. Network Topology (ESP-NOW)
We will use **ESP-NOW** protocol. It is connectionless, Wi-Fi independent, and extremely fast.
*   **Role A (Commander)**: The primary device connected to your phone via AP.
*   **Role B & C (Nodes)**: Devices placed in different locations.

### Feature 1: Synchronized Detonation
*   **Concept**: Triggering the relay on the Commander instantly triggers relays on Nodes B and C.
*   **Latency**: < 10ms.
*   **Use Case**: Simultaneous activation of multiple effects (lights, sound, locks) across a room.

### Feature 2: Tri-Pillar Security Perimeter
*   **Concept**: Each device acts as a sensor node (Sentinel).
*   **Logic**: If Node B detects movement (Gyro), it sends an alert to Commander. Commander alerts your phone.
*   **Coverage**: You can secure 3 different entry points (Door, Window, Safe).

### Feature 3: "Dead Man's Switch" Chain
*   **Concept**: Devices ping each other every second.
*   **Trigger**: If Node B goes offline (power cut/destroyed), Commander immediately triggers its relay (fail-safe alarm).

## 3. Implementation Plan (Future)
1.  **Phase 1**: Enable ESP-NOW on all devices. Hardcode MAC addresses or implement "Pairing Mode".
2.  **Phase 2**: Create a "Mesh Packet" structure (Type: SYNC, ALERT, STATUS).
3.  **Phase 3**: Update Android App to show status of all 3 devices in a list (Node 1, Node 2, Node 3).

## 4. Android UI Update Plan (Meshtastic Style)
To match the "Meshtastic" aesthetic:
*   **Navigation**: Switch to Bottom Navigation Bar (Dashboard | Map/Nodes | Settings).
*   **Dark Map**: Show relative signal strength distance radar.
*   **Node List**: Card-based list showing Battery, RSSI, and Status for each device.
