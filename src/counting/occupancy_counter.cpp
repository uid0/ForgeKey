#include "occupancy_counter.h"

OccupancyCounter occupancyCounter;

void OccupancyCounter::begin(int initialCount) {
    data.currentCount = initialCount;
    data.totalDetected = 0;
    data.lastUpdate = 0;
    data.changed = false;
    memset(detectionHistory, 0, sizeof(detectionHistory));
    historyIndex = 0;
}

void OccupancyCounter::updateCount(int detectedCount) {
    // Store in history for moving average
    detectionHistory[historyIndex] = detectedCount;
    historyIndex = (historyIndex + 1) % 5;
    
    int stableCount = calculateStableCount();
    
    if (stableCount != data.currentCount) {
        data.changed = true;
        data.currentCount = stableCount;
        data.lastUpdate = millis();
    } else {
        data.changed = false;
    }
}

int OccupancyCounter::calculateStableCount() {
    // Simple mode-based stabilization
    int counts[6] = {0};  // 0-5 people
    for (int i = 0; i < 5; i++) {
        if (detectionHistory[i] <= 5) {
            counts[detectionHistory[i]]++;
        }
    }
    
    int maxCount = 0;
    int maxVotes = 0;
    for (int i = 0; i <= 5; i++) {
        if (counts[i] > maxVotes) {
            maxVotes = counts[i];
            maxCount = i;
        }
    }
    
    return maxCount;
}

OccupancyData OccupancyCounter::getData() const {
    return data;
}

int OccupancyCounter::getCurrentCount() const {
    return data.currentCount;
}

void OccupancyCounter::reset() {
    begin(0);
}
