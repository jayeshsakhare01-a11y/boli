
#ifndef AUCTION_MANAGER_H
#define AUCTION_MANAGER_H

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vector>
#include <string>
#include <ctime>
#include <cstdio>
#include <iostream>
#include <cmath>

#include "bst.h"
#include "linked_list.h"
#include "queue.h"
#include "models.h"
#include "json_util.h"


// -- Currency helpers (Indian Rupees) ----------------------------------------
// Direct Buy ("reserve") price = 130% of the starting price, rounded.
// The frontend uses the exact same formula, so both sides always agree.
inline long long buyNowPriceFor(double startingPrice) {
    return (long long)std::llround(startingPrice * 1.3);
}

// Formats an amount with Indian digit grouping: 1105000 -> "₹11,05,000"
inline std::string formatINR(double amount) {
    long long v = (long long)std::llround(amount);
    std::string digits = std::to_string(v < 0 ? -v : v);
    std::string out;
    int n = (int)digits.size();
    if (n <= 3) {
        out = digits;
    } else {
        out = digits.substr(n - 3);
        int i = n - 3;
        while (i > 0) {
            int start = (i - 2 > 0) ? i - 2 : 0;
            out = digits.substr(start, i - start) + "," + out;
            i = start;
        }
    }
    return std::string(v < 0 ? "-" : "") + "\xE2\x82\xB9" + out;   // UTF-8 for the rupee sign
}

//  Notification - one entry in the FIFO event feed
struct Notification {
    std::string message;
    time_t      timestamp;
    int         relatedAuctionId;

    JsonObject toJson() const {
        JsonObject obj;
        obj.set("message", message)
           .set("timestamp", (long long)timestamp)
           .set("auctionId", relatedAuctionId);
        return obj;
    }
};


// AuctionManager - Central Auction Engine                                

class AuctionManager {
private:
    // -- Storage -----------------------------------------------------------
    std::vector<User*>    users;       // indexed by (id - 1)  [array/vector]
    std::vector<Auction*> auctions;    // indexed by (id - 1)  [array/vector]
    MinBST                expiryTree;  // global Min-BST for close-time tracking
    Queue<Notification>   notifications; // FIFO queue (capped at 200)

    // -- ID generators -----------------------------------------------------
    int nextUserId;
    int nextAuctionId;
    int nextBidId;

    // -- Thread safety -----------------------------------------------------
    CRITICAL_SECTION cs;               // reentrant mutex (Windows)

public:
    AuctionManager()
        : notifications(200), nextUserId(1), nextAuctionId(1), nextBidId(1)
    {
        InitializeCriticalSection(&cs);
    }

    ~AuctionManager() {
        for (size_t i = 0; i < users.size(); i++)    delete users[i];
        for (size_t i = 0; i < auctions.size(); i++) delete auctions[i];
        DeleteCriticalSection(&cs);
    }

    void lock()   { EnterCriticalSection(&cs); }
    void unlock() { LeaveCriticalSection(&cs); }

    //  USER OPERATIONS
    
    User* createUser(const std::string& name, const std::string& email) {
        lock();
        User* u  = new User();
        u->id    = nextUserId++;
        u->name  = name;
        u->email = email;
        users.push_back(u);

        Notification n;
        n.message          = "New user registered: " + name;
        n.timestamp        = time(NULL);
        n.relatedAuctionId = 0;
        notifications.enqueue(n);

        unlock();
        return u;
    }

    User* getUser(int id) {
        if (id < 1 || id > (int)users.size()) return NULL;
        return users[id - 1];
    }

    std::vector<User*> getAllUsers() {
        return users;
    }

    //  AUCTION OPERATIONS

    Auction* createAuction(const std::string& title, const std::string& desc,
                           const std::string& imageUrl, double startingPrice,
                           int durationSeconds)
    {
        lock();
        Auction* a      = new Auction();
        a->id           = nextAuctionId++;
        a->title        = title;
        a->description  = desc;
        a->imageUrl     = imageUrl;
        a->startingPrice= startingPrice;
        a->startTime    = time(NULL);
        a->endTime      = a->startTime + durationSeconds;
        a->isActive     = true;

        auctions.push_back(a);

        expiryTree.insert(a->endTime, a->id);

        Notification n;
        char buf[64];
        std::snprintf(buf, sizeof(buf), ", Duration: %ds)", durationSeconds);
        n.message          = "New auction created: \"" + title + "\" (Starting at "
                           + formatINR(startingPrice) + buf;
        n.timestamp        = time(NULL);
        n.relatedAuctionId = a->id;
        notifications.enqueue(n);

        std::cout << "[AUCTION] Created: " << title
                  << " | Ends at: " << a->endTime << std::endl;
        unlock();
        return a;
    }

    Auction* getAuction(int id) {
        if (id < 1 || id > (int)auctions.size()) return NULL;
        return auctions[id - 1];
    }

    std::vector<Auction*> getActiveAuctions() {
        std::vector<Auction*> result;
        for (size_t i = 0; i < auctions.size(); i++) {
            if (auctions[i]->isActive) result.push_back(auctions[i]);
        }
        return result;
    }

    std::vector<Auction*> getClosedAuctions() {
        std::vector<Auction*> result;
        for (size_t i = 0; i < auctions.size(); i++) {
            if (!auctions[i]->isActive) result.push_back(auctions[i]);
        }
        return result;
    }

    std::vector<Auction*> getAllAuctions() {
        return auctions;
    }

    //  BID PLACEMENT

    // Returns empty string on success, or an error message on failure.
    std::string placeBid(int auctionId, int userId, double amount) {
        lock();

        Auction* a = getAuction(auctionId);
        if (!a) { unlock(); return "Auction not found"; }
        if (!a->isActive) { unlock(); return "Auction is already closed"; }
        if (time(NULL) >= a->endTime) { unlock(); return "Auction has expired"; }

        User* u = getUser(userId);
        if (!u) { unlock(); return "User not found"; }

        // Validate: bid must exceed current highest (or starting price)
        double minBid = (a->highestBid > 0) ? a->highestBid : a->startingPrice;
        if (amount <= minBid) {
            std::string msg = "Bid must be higher than " + formatINR(minBid);
            unlock();
            return msg;
        }

        // -- Insert into the per-auction Max-BST --------------------------
        int bidId = nextBidId++;
        a->bidTree.insert(amount, bidId, userId, u->name, time(NULL));
        a->highestBid = amount;
        a->totalBids++;

        // -- Append to user's LinkedList bid history ----------------------
        BidRecord record;
        record.auctionId    = auctionId;
        record.auctionTitle = a->title;
        record.amount       = amount;
        record.timestamp    = time(NULL);
        record.isWinning    = false;
        u->bidHistory.append(record);

        // -- Notification -------------------------------------------------
        Notification n;
        n.message          = u->name + " placed a bid of " + formatINR(amount)
                           + " on \"" + a->title + "\"";
        n.timestamp        = time(NULL);
        n.relatedAuctionId = auctionId;
        notifications.enqueue(n);

        std::cout << "[BID] " << u->name << " -> Rs " << (long long)amount
                  << " on \"" << a->title << "\"" << std::endl;

        unlock();
        return "";   // success
    }

    // =====================================================================
    //  DIRECT BUY - instant purchase at the reserve (Direct Buy) price
    // =====================================================================
    //  Records the purchase as the winning bid in the auction's Max-BST,
    //  closes the auction immediately and settles at the Direct Buy price
    //  (no Vickrey discount - the buyer pays the reserve).
    //  The expiry Min-BST entry is left in place; checkAndCloseExpired()
    //  skips auctions that are already closed.
    //  Returns "" on success, otherwise an error message.
    std::string buyNow(int auctionId, int userId, double amount, double& settledOut) {
        lock();

        Auction* a = getAuction(auctionId);
        if (!a) { unlock(); return "Auction not found"; }
        if (!a->isActive || time(NULL) >= a->endTime) {
            unlock();
            return "Auction is already closed";
        }

        User* u = getUser(userId);
        if (!u) { unlock(); return "User not found"; }

        double price = (double)buyNowPriceFor(a->startingPrice);
        if (amount + 0.5 < price) {
            std::string msg = "Direct Buy price is " + formatINR(price);
            unlock();
            return msg;
        }
        if (a->highestBid >= price) {
            unlock();
            return "Current bid has already reached the Direct Buy price - place a higher bid instead";
        }

        // Winning bid goes into the Max-BST like any other bid
        int bidId = nextBidId++;
        a->bidTree.insert(price, bidId, userId, u->name, time(NULL));
        a->highestBid   = price;
        a->totalBids++;

        // Close + settle immediately
        a->isActive     = false;
        a->winnerId     = userId;
        a->winnerName   = u->name;
        a->settledPrice = price;

        // Buyer's LinkedList history
        BidRecord record;
        record.auctionId    = auctionId;
        record.auctionTitle = a->title;
        record.amount       = price;
        record.timestamp    = time(NULL);
        record.isWinning    = true;
        u->bidHistory.append(record);

        Notification n;
        n.message          = "DIRECT BUY: " + u->name + " purchased \"" + a->title
                           + "\" for " + formatINR(price);
        n.timestamp        = time(NULL);
        n.relatedAuctionId = auctionId;
        notifications.enqueue(n);

        std::cout << "[BUYNOW] " << u->name << " bought \"" << a->title
                  << "\" for Rs " << (long long)price << std::endl;

        settledOut = price;
        unlock();
        return "";
    }

    // =====================================================================
    //  AUTOMATED EXPIRY - Called by the background thread every second
    // =====================================================================

    void checkAndCloseExpired() {
        lock();

        time_t now = time(NULL);

        // Keep draining the Min-BST as long as the leftmost node has expired
        while (true) {
            time_t closeTime;
            int    auctionId;
            if (!expiryTree.peekMin(closeTime, auctionId)) break;
            if (closeTime > now) break;   // nothing expired yet

            // -- Remove from Min-BST --------------------------------------
            expiryTree.removeMin();

            Auction* a = getAuction(auctionId);
            if (!a || !a->isActive) continue;

            // -- Close the auction ----------------------------------------
            a->isActive = false;
            std::cout << "\n[EXPIRY] Closing auction: " << a->title << std::endl;

            if (a->bidTree.isEmpty()) {
                // No bids were placed
                Notification n;
                n.message = "Auction \"" + a->title + "\" closed with no bids.";
                n.timestamp        = now;
                n.relatedAuctionId = auctionId;
                notifications.enqueue(n);
                std::cout << "  -> No bids. Auction ended without a winner.\n";
                continue;
            }

            // -- Vickrey (Second-Price) Settlement ------------------------
            //  1. Rightmost node of Max-BST  -> highest bidder (WINNER)
            //  2. In-order predecessor of max -> second-highest (PRICE)
            BidBSTNode* maxNode = a->bidTree.findMax();
            a->winnerId   = maxNode->data.userId;
            a->winnerName = maxNode->data.userName;
            a->highestBid = maxNode->data.amount;

            BidBSTNode* secondMax = a->bidTree.findSecondMax();
            if (secondMax && secondMax != maxNode) {
                a->settledPrice = secondMax->data.amount;   // Vickrey price
            } else {
                a->settledPrice = maxNode->data.amount;     // only one bidder
            }

            // -- Update winner's bid history ------------------------------
            User* winner = getUser(a->winnerId);
            if (winner) {
                for (int i = 0; i < winner->bidHistory.size(); i++) {
                    BidRecord& rec = winner->bidHistory.get(i);
                    if (rec.auctionId == auctionId) {
                        rec.isWinning = true;
                    }
                }
            }

            // -- Notification ---------------------------------------------
            Notification n;
            n.message          = "AUCTION CLOSED: \"" + a->title + "\" | Winner: " + a->winnerName
                               + " | Highest Bid: " + formatINR(a->highestBid)
                               + " | Settled Price (Vickrey): " + formatINR(a->settledPrice)
                               + " | Total Bids: " + std::to_string(a->totalBids);
            n.timestamp        = now;
            n.relatedAuctionId = auctionId;
            notifications.enqueue(n);

            std::cout << "  -> Winner: " << a->winnerName
                      << " | Highest Bid: Rs " << (long long)a->highestBid
                      << " | Vickrey Price: Rs " << (long long)a->settledPrice
                      << " | Bids: " << a->totalBids << std::endl;
        }

        unlock();
    }

    // =====================================================================
    //  NOTIFICATIONS
    // =====================================================================

    std::vector<Notification> getNotifications() {
        lock();
        std::vector<Notification> result = notifications.toVector();
        unlock();
        return result;
    }

    // =====================================================================
    //  SEED DEMO DATA - pre-populates users, auctions, and bids for demo
    // =====================================================================

    void seedDemoData() {
        std::cout << "[SEED] Creating demo users...\n";
        createUser("Alice Johnson",  "alice@example.com");
        createUser("Bob Smith",      "bob@example.com");
        createUser("Charlie Brown",  "charlie@example.com");
        createUser("Diana Prince",   "diana@example.com");
        createUser("Ethan Hunt",     "ethan@example.com");

        std::cout << "[SEED] Creating demo vehicle auctions...\n";

        // Descriptions use the pipe-separated format the frontend parses:
        // "Fuel: .. | Trans: .. | .. km | .. Owner | RTO: <code> <city> | highlights..."
        createAuction(
            "2021 Hyundai Creta 1.5 SX(O)",
            "Fuel: Petrol | Trans: Manual | 34,200 km | 1st Owner | RTO: MH12 Pune | Comprehensive Insurance | Zero Dep | 140-point inspection passed | Clean engine bay and transmission | Genuine service records",
            "https://images.unsplash.com/photo-1549399542-7e3f8b79c341?w=800&auto=format&fit=crop",
            850000, 240);    // 4 min

        createAuction(
            "2022 Royal Enfield Continental GT 650",
            "Fuel: Petrol | Trans: Manual | 8,400 km | 1st Owner | RTO: MH14 PCMC | Comprehensive Insurance | Twin Brembo brakes | Custom flyscreen & bar-end mirrors | Ceramic coated",
            "https://images.unsplash.com/photo-1558981403-c5f9899a28bc?w=800&auto=format&fit=crop",
            240000, 360);    // 6 min

        createAuction(
            "2020 Honda City 1.5 ZX CVT",
            "Fuel: Petrol | Trans: Automatic | 41,000 km | 1st Owner | RTO: MH02 Mumbai | Full Sunroof & Leather | Honda Care Extended Warranty | Non-accidental certified",
            "https://images.unsplash.com/photo-1605559424843-9e4c228bf1c2?w=800&auto=format&fit=crop",
            720000, 600);    // 10 min

        createAuction(
            "2023 Kawasaki Ninja ZX-6R KRT",
            "Fuel: Petrol | Trans: Manual | 5,200 km | 1st Owner | RTO: DL03 Delhi | Showroom condition | Akrapovic slip-on exhaust | Quickshifter & traction control",
            "https://images.unsplash.com/photo-1568772585407-9361f9bf3a87?w=800&auto=format&fit=crop",
            980000, 900);    // 15 min

        createAuction(
            "2019 BMW 3 Series 320d Luxury Line",
            "Fuel: Diesel | Trans: Automatic | 52,000 km | 1st Owner | RTO: KA01 Bangalore | Harman Kardon audio | Panoramic sunroof | New Michelin Pilot Sport tyres",
            "https://images.unsplash.com/photo-1555215695-3004980ad54e?w=800&auto=format&fit=crop",
            1850000, 1200);  // 20 min

        createAuction(
            "2022 Tata Nexon EV Max XZ+ Lux",
            "Fuel: Electric | Trans: Automatic | 22,000 km | 1st Owner | RTO: GJ01 Ahmedabad | 405 km ARAI range | 7.2 kW Fast AC charger included | 8-year battery warranty",
            "https://images.unsplash.com/photo-1590362891991-f776e747a588?w=800&auto=format&fit=crop",
            1050000, 1800);  // 30 min

        std::cout << "[SEED] Placing initial demo bids...\n";

        placeBid(1, 2,  880000);   // Bob     -> Creta
        placeBid(1, 3,  900000);   // Charlie -> Creta
        placeBid(1, 4,  925000);   // Diana   -> Creta

        placeBid(2, 1,  255000);   // Alice   -> Continental GT
        placeBid(2, 5,  265000);   // Ethan   -> Continental GT

        placeBid(3, 2,  740000);   // Bob     -> City
        placeBid(3, 4,  755000);   // Diana   -> City

        placeBid(4, 3, 1005000);   // Charlie -> Ninja
        placeBid(4, 5, 1030000);   // Ethan   -> Ninja

        placeBid(5, 1, 1890000);   // Alice   -> BMW
        placeBid(5, 2, 1920000);   // Bob     -> BMW

        placeBid(6, 4, 1075000);   // Diana   -> Nexon EV

        std::cout << "[SEED] Demo data ready! "
                  << users.size()    << " users, "
                  << auctions.size() << " auctions, "
                  << (nextBidId - 1) << " bids.\n\n";
    }
};

#endif // AUCTION_MANAGER_H
