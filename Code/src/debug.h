#include <Arduino.h>


#ifdef ENABLE_HEAP_DEBUGGING
    #define HEAP_DEBUG_MESSAGE(location) printHeapDebugData(location);
#else
    #define HEAP_DEBUG_MESSAGE(location) 
#endif

inline void printHeapDebugData(const char *location){
    Serial.println("Heap: " + String(ESP.getMinFreeHeap()/1024) + "\t" + String(ESP.getFreeHeap()/1024) + "\t" + String(ESP.getMaxAllocHeap()/1024) + "\t" + location);
}

class PerfTracer {
    const char* name;
    unsigned long start;
public:
    PerfTracer(const char* n) : name(n), start(millis()) {
        // Serial.printf("[PERF_START] %s\n", name); // Optional: Uncomment if start logs are needed
    }
    ~PerfTracer() {
        unsigned long duration = millis() - start;
        if (duration > 5) // Log if takes more than 5ms
            Serial.printf("[PERF_DEBUG] %s took %lu ms\n", name, duration);
    }
};

#define PROFILE_SCOPE(name) PerfTracer perfTracer(name)
#define PROFILE_FUNCTION() PROFILE_SCOPE(__PRETTY_FUNCTION__)
