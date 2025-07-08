#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <optional>
#include <chrono>
#include <uuid/uuid.h>
using namespace std;


/*
2. Main APIs: (All APIs are thread-safe and these APIs can be directly called by the user/user facing function)
  createBooking(roomId, start, end, cap, emails) -> BookingId, Notifications Sent
  findAvailableRooms(start, end, cap) -> List of RoomIds
  cancelBooking(bookingId) -> Notifications Sent
  listCalenderForDay(roomId, day) -> List of Bookings
*/

/*
3. Data Models: to store in the database the above APIs results
  Room
  - id
  - capacity

  Bookings
  - id
  - roomId
  - start
  - end
  - List[emails]
  - createdAt

  Attendees
  -id
  -email
  -bookingId
*/

/*
4. Services:
   MeetingService
   - Book()
   - Cancel()
   - ListCalender()
   - findAvailableRooms()

   NotificationService
   - sendInvites()
   - sendCancellations()

   CalendarService
   - isFree()
   - addEntry()
   - removeEntry()
*/

/*
 Flow:
   Book()
   - findAvailableRooms()
   - selectRoom()
   - createBooking()
   - sendInvites()   
*/

/*
 Wrap DB write(BookingRepository save) + Calendar Update(calenderService.addEntry) in a single ACID Transaction to handle concurrency 
*/


// ——— Domain Models ———
struct Room {
    string id;
    int capacity;
};

struct Booking {
    string id;
    string roomId;
    chrono::system_clock::time_point start;
    chrono::system_clock::time_point end;
    vector<string> attendees;
};

// ——— Repositories ———
class RoomRepository {
public:
    vector<Room> findByCapacity(int cap);
    optional<Room> findById(const string&); // std::optional<T> is a smart pointer 
    // that can be empty returns a valid object if found else returns an empty optional for exception handling
private:
    vector<Room> rooms_;
};

class BookingRepository {
public:
    void save(const Booking&);
    optional<Booking> findById(const string&);
    void remove(const string&);
    vector<Booking> findByRoomAndDay(const string&, 
        const tm& date);
private:
    map<string, Booking> bookings_;
    shared_mutex mtx_;
};

// ——— Services ———
class CalendarService {
public:
    // Singleton pattern -> Insures only one instance of the class is every constructed and shared across threads
    static CalendarService& instance() {
        static CalendarService inst;
        return inst;
    }
    // Encapsulates conflict logic (overlaps) and delegates storage to BookingRepository.
    bool isFree(const string& roomId,
                const Booking& b) {
        auto day = toDate(b.start);
        for (auto& ex : repo_.findByRoomAndDay(roomId, day)) {
            if (overlaps(ex, b)) return false;
        }
        return true;
    }
    // Encapsulates storage logic and delegates to BookingRepository.
    void addEntry(const Booking& b) {
        repo_.save(b);
    }
    // Encapsulates storage logic and delegates to BookingRepository.
    void removeEntry(const string& bookingId) {
        repo_.remove(bookingId);
    }
private:
    BookingRepository repo_;
    bool overlaps(const Booking&a, const Booking&b) {
        return !(b.end <= a.start || b.start >= a.end);
    }
    tm toDate(const chrono::system_clock::time_point&);
};

// A simple façade over whatever email/SMS system you choose.
class NotificationService {
public:
    static NotificationService& instance() {
        static NotificationService inst;
        return inst;
    }
    void sendInvites(const vector<string>& users,
                     const Booking& b) {
        // fire off email/SMS...
    }
    void sendCancellations(const vector<string>& users,
                           const Booking& b) {
        // ...
    }
};

// ——— Room Allocation Strategy. This is in an interface to allow for different strategies to be used like FirstFit and BestFit ———
class IRoomStrategy {
public:
    virtual optional<Room> select(
        const vector<Room>&, int) = 0;
};
// Finds the smallest room whose capacity ≥ requested headcount.
class SmallestFitStrategy : public IRoomStrategy {
    optional<Room> select(const vector<Room>& rooms,
                               int cap) override {
        optional<Room> best;
        for (auto& r : rooms) {
            if (r.capacity >= cap &&
               (!best || r.capacity < best->capacity))
                best = r;
        }
        return best;
    }
};

// ——— Meeting Service ———
class MeetingService {
public:
    MeetingService(RoomRepository& rr,
                   BookingRepository& br,
                   IRoomStrategy& strat)
     : rr_(rr), br_(br), strat_(strat) {}

    optional<string> bookMeeting(
        const Booking& req) 
    {
        auto rooms = rr_.findByCapacity(req.attendees.size());
        auto room = strat_.select(rooms, req.attendees.size());
        if (!room) return nullopt;

        Booking b = req;
        b.roomId = room->id;
        b.id = newUUID();

        auto& cal = CalendarService::instance();
        // Locks the mutex associated with a specific room ID, preventing two threads from booking the same room at once.
        unique_lock lock(mutexes_[b.roomId]);
        if (!cal.isFree(b.roomId, b)) return nullopt;

        // Atomic-ish sequence: save booking + update calendar
        br_.save(b);
        cal.addEntry(b);
        NotificationService::instance()
            .sendInvites(b.attendees, b);
        return b.id;
    }

    bool cancelMeeting(const string& id) {
        auto opt = br_.findById(id);
        if (!opt) return false;
        auto b = *opt;
        br_.remove(id);
        CalendarService::instance().removeEntry(id);
        NotificationService::instance()
            .sendCancellations(b.attendees, b);
        return true;
    }

private:
    RoomRepository& rr_;
    BookingRepository& br_;
    IRoomStrategy& strat_;
    map<string, mutex> mutexes_;

    string newUUID() {
        uuid_t u; uuid_generate(u);
        char s[37]; uuid_unparse(u, s);
        return string{s};
    }
};

/*
Thread safety:

    You pick a per-room mutex so two threads can still book different rooms in parallel.

    You wrap the critical “check + write” section under that lock.

Failure modes:

    If no room is available or it’s already booked, you return std::nullopt.

    Caller sees “no booking ID,” so they know it failed.
*/