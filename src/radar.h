#ifndef RADAR_H
#define RADAR_H


#include <stdint.h>
#include <CoreMutex.h>
#include <ld2410.h>


class Radar {
public:
    typedef struct target {
    bool detected;
    uint8_t energy;
    uint16_t distance_cm;
    } target_t;

    typedef struct status {
        bool connected;
        bool presence;
        target_t stationary;
        target_t moving;
    } status_t;

    Radar();

    void update( ld2410 &radar );
    void get( status_t &status );

private:
  status_t _status;
  mutex_t _mutex;
};


bool operator!=( const Radar::status_t& l, const Radar::status_t& r );


extern Radar RadarStatus;


#endif
