# Boli — Real-Time Vehicle Auction Platform
### Fundamentals of Data Structures (FDS) Course Project

---

## 📌 Project Overview & Team Information
- **Project Title:** Boli — Real-Time Verified Vehicle & Asset Auctions
- **Course:** Fundamentals of Data Structures (FDS)
- **Academic Year:** 2025–2026
- **Institution:** Vishwakarma Institute of Technology (VIT) / VIIT Pune
- **Project Guide:** Prof. Suruchi Dedgaonkar (`suruchi.dedgaonkar@vit.edu` / `suruchi.dedgaonkar@viit.ac.in`)
- **GitHub Repository:** [https://github.com/jayeshsakhare01-a11y/boli](https://github.com/jayeshsakhare01-a11y/boli)
- **Team Members:**
  - Jayesh Sakhare
  - [Team Member 2 Name] (Roll No: [Roll No])
  - [Team Member 3 Name] (Roll No: [Roll No])

---

## 🎯 Problem Statement & Motivation
Online auction environments experience intense bursts of write and read operations. Rapidly updating highest bids, calculating settlement prices, and automatically closing expired auctions using traditional relational database queries (`ORDER BY bid DESC LIMIT 2`) creates significant query latency and overhead.

**Objective:**  
To design and build an in-memory, high-throughput auction engine from scratch in C++ that demonstrates the practical application of custom data structures—without relying on high-level standard libraries (STL heaps/priority queues) or third-party web frameworks.

---

## 🏗️ Data Structures Mapping & Complexity

The system implements four custom data structures from foundational principles:

| Data Structure | Implementation File | Key Role in System | Time Complexity |
|---|---|---|---|
| **Max-BST** | `backend/include/bst.h` | Per-auction bid tree keyed by bid amount. Rightmost node represents the winning bid; in-order predecessor yields the 2nd-highest (Vickrey) price. | Insert: $O(\log N)$<br>Find Winner: $O(h)$<br>In-order list: $O(N)$ |
| **Min-BST** | `backend/include/bst.h` | Global auction expiry tracker keyed by expiration timestamp (epoch seconds). Leftmost node represents the soonest expiring auction. | Insert: $O(\log N)$<br>Peek Soonest: $O(1)$<br>Remove Expired: $O(\log N)$ |
| **Singly Linked List** | `backend/include/linked_list.h` | Maintains chronological bid history per user (`User::bidHistory`). | Append: $O(1)$<br>Traversal: $O(N)$ |
| **FIFO Queue** | `backend/include/queue.h` | Event log queue (`AuctionManager::notifications`) that stores real-time audit notifications and broadcasts events to the UI. | Enqueue: $O(1)$<br>Dequeue: $O(1)$ |

---

## ⚙️ System Architecture & Concurrency

```
                        ┌─────────────────────────────────────┐
                        │   Frontend SPA (Browser / Client)   │
                        └──────────────────┬──────────────────┘
                                           │ HTTP/1.1 (JSON)
                                           ▼
                        ┌─────────────────────────────────────┐
                        │  Custom Winsock HTTP Server (:8080) │
                        └──────────────────┬──────────────────┘
                                           │
                    ┌──────────────────────┴──────────────────────┐
                    ▼                                             ▼
        ┌───────────────────────┐                     ┌───────────────────────┐
        │    REST API Routes    │                     │ Expiry Worker Thread  │
        │  (Bids, Lots, Users)  │                     │  (1-sec Polling Loop) │
        └───────────┬───────────┘                     └───────────┬───────────┘
                    │                                             │
                    └───────────────┬─────────────────────────────┘
                                    │ Thread Synchronization
                                    │ (CRITICAL_SECTION Mutex)
                                    ▼
                    ┌─────────────────────────────────┐
                    │       AuctionManager Core       │
                    │  ├── MaxBST (Per-Auction Bids)  │
                    │  ├── MinBST (Auction Expiry)    │
                    │  ├── LinkedList (User History)  │
                    │  └── Queue (Notification Feed)  │
                    └─────────────────────────────────┘
```

### Key Technical Highlights:
1. **Thread-Safe Concurrency:**  
   The background worker thread (`expiryThreadFunc`) continuously checks the **Min-BST** every second to settle expired lots. Access to shared memory is guarded by Windows `CRITICAL_SECTION` mutexes (`lock()` and `unlock()`) to prevent race conditions during concurrent bid submissions.
2. **Zero-Dependency Winsock Server:**  
   The HTTP server is built natively on `winsock2.h` with socket pooling, manual HTTP request parsing, JSON serialization, and static asset streaming.
3. **Vickrey Auction Model:**  
   Supports both standard English auction bidding and second-price Vickrey settlement, where the highest bidder wins but only pays the second-highest bid plus an incremental tick.
4. **Direct Buy (Buy-Now):**  
   Provides immediate vehicle acquisition settled at a fixed threshold (130% of base price), instantaneously removing the lot from the active **Min-BST**.

---

## 📡 REST API Reference

All requests and responses use standard JSON formatting:

| Method | Endpoint | Description |
|---|---|---|
| `GET` | `/api/users` | List all registered demo users |
| `GET` | `/api/users/:id` | Retrieve user details and associated `LinkedList` bid history |
| `POST` | `/api/users` | Register a new user (`{ name, email }`) |
| `GET` | `/api/auctions` | List all active vehicle auctions |
| `GET` | `/api/auctions/:id` | Fetch detailed auction metadata and in-order `Max-BST` bid ranking |
| `POST` | `/api/auctions` | Create a new auction (`{ title, description, imageUrl, startingPrice, durationSeconds }`) |
| `POST` | `/api/auctions/:id/bid` | Submit a competitive bid (`{ userId, amount }`) |
| `POST` | `/api/auctions/:id/buynow`| Instantly purchase via Direct Buy (`{ userId, amount }`) |
| `GET` | `/api/closed` | List closed and settled auctions with Vickrey price breakdowns |
| `GET` | `/api/notifications` | Fetch recent system notifications from the `Queue` |

---

## 🚀 Build & Run Instructions

### Prerequisites
- Windows OS (Windows 10/11)
- MinGW C++ Compiler (`g++` supporting C++14 or higher)

### 1. Compile & Start Backend
Open a terminal (PowerShell or Command Prompt) and run:
```powershell
cd backend
.\build.bat run
```
*Alternatively using `g++` directly:*
```powershell
cd backend
g++ -std=c++14 -O2 -Wall -Wextra -I include -o bidmatrix.exe src\main.cpp -lws2_32
.\bidmatrix.exe
```

### 2. Open the Web Application
Open your browser and navigate to:
```
http://localhost:8080
```
The backend automatically hosts the frontend SPA and routes all API endpoints on port `8080`.

---

## 📂 Project Directory Structure

```
fds_project/
├── backend/
│   ├── include/
│   │   ├── auction_manager.h    # Core business logic & synchronization
│   │   ├── bst.h                # Custom Max-BST (Bids) & Min-BST (Expiry)
│   │   ├── http_server.h        # Winsock HTTP/1.1 socket server
│   │   ├── json_util.h          # Lightweight JSON serializer & parser
│   │   ├── linked_list.h        # Custom Singly-Linked List template
│   │   ├── models.h             # Data entities (User, Auction, Bid)
│   │   └── queue.h              # Custom FIFO Queue template
│   ├── src/
│   │   └── main.cpp             # Application entry, routes & expiry thread
│   ├── Makefile                 # Linux/MinGW makefile
│   └── build.bat                # Windows build script
├── frontend/
│   ├── images/                  # Vehicle lot media assets & brand logo
│   └── index.html               # Single Page Application (HTML5/CSS3/Vanilla JS)
├── .gitignore                   # Excludes binaries and IDE files
└── README.md                    # Project documentation
```
