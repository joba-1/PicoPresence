#include <radar.h>
#include <string.h>


Radar::Radar() : _status({0}) {
    mutex_init(&_mutex);
}


void Radar::update( ld2410 &radar ) {
    CoreMutex m(&_mutex);

    // status always contains latest valid values
    _status.connected = radar.isConnected();
    if (_status.connected) {
        _status.presence = radar.presenceDetected();
        if (_status.presence) {
            _status.stationary.detected = radar.stationaryTargetDetected();
            if (_status.stationary.detected) {
                _status.stationary.distance_cm = radar.stationaryTargetDistance();
                _status.stationary.energy = radar.stationaryTargetEnergy();
            }
            _status.moving.detected = radar.movingTargetDetected();
            if (_status.moving.detected) {
                _status.moving.distance_cm = radar.movingTargetDistance();
                _status.moving.energy = radar.movingTargetEnergy();
            }
        }
    }
}


void Radar::get( status_t &status ) {
    CoreMutex m(&_mutex);

    status = _status;
}


bool operator!=( const Radar::status_t& l, const Radar::status_t& r ) {
    return memcmp(&l, &r, sizeof(Radar::status_t)) != 0;
}


Radar RadarStatus;
