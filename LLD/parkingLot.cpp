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
 1) APIs: (All APIs are thread-safe and can be called directly by clients)
    - createParkingLot(levels, spotsPerLevel, spotTypeCounts) -> ParkingLotId
    - parkVehicle(vehicleId, vehicleType) -> optional<ParkingSpotId>
    - leaveVehicle(vehicleId) -> bool
    - getAvailableSpots(vehicleType) -> List<ParkingSpotId>
*/

/*
 2) Data Models: how things are stored

   // static descriptions
   struct ParkingLot {
     string id;
     int numLevels;
     // fee rules, etc.
   };

   struct ParkingFloor {
     string id;
     string lotId;
     int level;
   };

   enum class VehicleType { Motorcycle, Car, Truck };

   enum class SpotType { Motorcycle, Compact, Large };

   struct ParkingSpot {
     string id;
     string floorId;
     SpotType type;
   };

   // dynamic assignments
   struct Booking {
     string id;
     string spotId;
     string vehicleId;
     VehicleType vehicleType;
     chrono::system_clock::time_point start;
     // end == leave time
     optional<chrono::system_clock::time_point> end;
   };
*/

/*
 3) Repositories: handle all data access (no business logic here)
*/

class ParkingLotRepository {
public:
  void save(const ParkingLot& lot) {
    unique_lock lock(mtx_);
    lots_[lot.id] = lot;
  }
  optional<ParkingLot> findById(const string& id) {
    shared_lock lock(mtx_);
    auto it = lots_.find(id);
    if (it == lots_.end()) return nullopt;
    return it->second;
  }
private:
  map<string, ParkingLot> lots_;
  shared_mutex mtx_;
};

class ParkingFloorRepository {
public:
  void save(const ParkingFloor& f) {
    unique_lock lock(mtx_);
    floors_[f.id] = f;
  }
  vector<ParkingFloor> findByLot(const string& lotId) {
    shared_lock lock(mtx_);
    vector<ParkingFloor> res;
    for (auto& [_, f]: floors_)
      if (f.lotId == lotId) res.push_back(f);
    return res;
  }
private:
  map<string, ParkingFloor> floors_;
  shared_mutex mtx_;
};

class ParkingSpotRepository {
public:
  void save(const ParkingSpot& s) {
    unique_lock lock(mtx_);
    spots_[s.id] = s;
  }
  vector<ParkingSpot> findByFloor(const string& floorId) {
    shared_lock lock(mtx_);
    vector<ParkingSpot> res;
    for (auto& [_, s]: spots_)
      if (s.floorId == floorId) res.push_back(s);
    return res;
  }
  optional<ParkingSpot> findById(const string& id) {
    shared_lock lock(mtx_);
    auto it = spots_.find(id);
    if (it==spots_.end()) return nullopt;
    return it->second;
  }
private:
  map<string, ParkingSpot> spots_;
  shared_mutex mtx_;
};

class BookingRepository {
public:
  void save(const Booking& b) {
    unique_lock lock(mtx_);
    bookings_[b.id] = b;
    vehicleIndex_[b.vehicleId] = b.id;
  }
  optional<Booking> findByVehicle(const string& vehicleId) {
    shared_lock lock(mtx_);
    auto it = vehicleIndex_.find(vehicleId);
    if (it==vehicleIndex_.end()) return nullopt;
    return bookings_[it->second];
  }
  void remove(const string& bookingId) {
    unique_lock lock(mtx_);
    auto it = bookings_.find(bookingId);
    if (it!=bookings_.end()) {
      vehicleIndex_.erase(it->second.vehicleId);
      bookings_.erase(it);
    }
  }
private:
  map<string, Booking> bookings_;
  map<string, string> vehicleIndex_;
  shared_mutex mtx_;
};

/*
 4) Services: implement business logic, delegate persistence to Repos, use per-spot locks
*/

class ParkingService {
public:
  ParkingService(ParkingLotRepository& lr,
                 ParkingFloorRepository& fr,
                 ParkingSpotRepository& sr,
                 BookingRepository& br)
    : lotRepo_(lr), floorRepo_(fr), spotRepo_(sr), bookingRepo_(br) {}

  // create the lot structure
  string createParkingLot(int levels, int spotsPerLevel,
                          map<SpotType,int> spotTypeCounts) 
  {
    string lotId = newUUID();
    ParkingLot lot{lotId, levels};
    lotRepo_.save(lot);

    // for each level...
    for (int lvl=1; lvl<=levels; ++lvl) {
      string floorId = newUUID();
      floorRepo_.save(ParkingFloor{floorId, lotId, lvl});
      // create spots of each type
      for (auto& [type,count]: spotTypeCounts) {
        for (int i=0; i<count; ++i) {
          string spotId = newUUID();
          spotRepo_.save(ParkingSpot{spotId, floorId, type});
          // initialize lock for this spot
          mutexes_[spotId];
        }
      }
    }
    return lotId;
  }

  optional<string> parkVehicle(const string& vehicleId,
                               VehicleType vt) 
  {
    // simple first-fit: scan all spots, pick first free that matches
    // In production, you’d index by type for O(1) lookup
    for (auto& [spotId, mtx] : mutexes_) {
      // lock per-spot to avoid races
      unique_lock lock(mtx, try_to_lock);
      if (!lock || isOccupied(spotId)) continue;
      // check spot type fits vehicle
      auto spOpt = spotRepo_.findById(spotId);
      if (!spOpt) continue;
      if (!fits(vt, spOpt->type)) continue;

      // allocate
      auto now = chrono::system_clock::now();
      Booking b{newUUID(), spotId, vehicleId, vt, now, nullopt};
      bookingRepo_.save(b);
      return b.id;
    }
    return nullopt; // full
  }

  bool leaveVehicle(const string& vehicleId) {
    auto bop = bookingRepo_.findByVehicle(vehicleId);
    if (!bop) return false;
    Booking b = *bop;
    // mark end time (optional if you want history)
    b.end = chrono::system_clock::now();
    bookingRepo_.remove(b.id);
    return true;
  }

  vector<string> getAvailableSpots(VehicleType vt) {
    vector<string> res;
    for (auto& [spotId, _] : mutexes_) {
      if (isOccupied(spotId)) continue;
      auto sp = spotRepo_.findById(spotId).value();
      if (fits(vt, sp.type)) res.push_back(spotId);
    }
    return res;
  }

private:
  bool isOccupied(const string& spotId) {
    // simple check: any booking for this spot without end?
    // For brevity, we assume one active booking per spot
    // In production, index bookings by spotId
    auto all = bookingRepo_.findByVehicle(""); // not ideal
    // … implement proper lookup …
    return false;
  }

  bool fits(VehicleType v, SpotType s) {
    switch(v) {
      case VehicleType::Motorcycle:
        return true;              // motorcycles fit anywhere
      case VehicleType::Car:
        return s==SpotType::Compact || s==SpotType::Large;
      case VehicleType::Truck:
        return s==SpotType::Large;
    }
    return false;
  }

  string newUUID() {
    uuid_t u; uuid_generate(u);
    char buf[37]; uuid_unparse(u, buf);
    return string{buf};
  }

  ParkingLotRepository& lotRepo_;
  ParkingFloorRepository& floorRepo_;
  ParkingSpotRepository& spotRepo_;
  BookingRepository& bookingRepo_;

  // per-spot locks
  map<string, mutex> mutexes_;
};

/*
 5) Flow:
    - client calls createParkingLot(...) once
    - on entry: parkVehicle(id, type) → assigns first-fit free spot or returns none
    - on exit: leaveVehicle(id) → frees the spot
    - optionally: getAvailableSpots(type) for a dashboard
*/

/* Thread-safety & scaling notes:
   - We use one mutex per spot so two cars can park in different spots concurrently.
   - The “check + allocate” is done under that spot’s lock.
   - Repositories use shared_mutex to allow concurrent reads.
   - For high throughput, index free-spot lists by type and maintain a concurrent free-list.
*/

int main() {
  // example usage
  ParkingLotRepository lotRepo;
  ParkingFloorRepository floorRepo;
  ParkingSpotRepository spotRepo;
  BookingRepository bookingRepo;
  ParkingService svc(lotRepo, floorRepo, spotRepo, bookingRepo);

  // create a 3-level lot, 10 spots each: 2 motorcycle, 6 compact, 2 large per level
  map<SpotType,int> counts{{SpotType::Motorcycle,2},
                           {SpotType::Compact,6},
                           {SpotType::Large,2}};
  string lotId = svc.createParkingLot(3, 10, counts);

  auto booking = svc.parkVehicle("KA01AB1234", VehicleType::Car);
  if (booking) cout << "Parked in booking " << *booking << "\n";
  else         cout << "Lot full!\n";

  svc.leaveVehicle("KA01AB1234");
  return 0;
}
