#ifndef OCCUPANCY_COUNTER_H
#define OCCUPANCY_COUNTER_H

#include <Arduino.h>

struct OccupancyData {
    int currentCount;
    int totalDetected;
    unsigned long lastUpdate;
    bool changed;
};

class OccupancyCounter {
public:
    void begin(int initialCount = 0);
    void updateCount(int detectedCount);
    OccupancyData getData() const;
    int getCurrentCount() const;
    void reset();
    
private:
    OccupancyData data;
    int detectionHistory[5] = {0};  // Moving average buffer
    int historyIndex = 0;
    
    int calculateStableCount();
};

extern OccupancyCounter occupancyCounter;

#endif
