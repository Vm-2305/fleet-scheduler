#ifndef SHARED_H
#define SHARED_H
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>

#define MAX_TRUCKS          250
#define TRUCK_MAX_CAP        20
#define MAX_NEW_REQUESTS     50
#define MAX_TOTAL_PACKAGES 5000

typedef struct {
    int packageId;
    int pickup_x,  pickup_y;
    int dropoff_x, dropoff_y;
    int arrival_turn;
    int expiry_turn;
} PackageRequest;

typedef struct {
    char authStrings[MAX_TRUCKS][TRUCK_MAX_CAP + 1];
    char truckMovementInstructions[MAX_TRUCKS];
    int  pickUpCommands[MAX_TRUCKS];
    int  dropOffCommands[MAX_TRUCKS];
    int  truckPositions[MAX_TRUCKS][2];
    int  truckPackageCount[MAX_TRUCKS];
    int  truckTurnsInToll[MAX_TRUCKS];
    PackageRequest newPackageRequests[MAX_NEW_REQUESTS];
    int  packageLocations[MAX_TOTAL_PACKAGES][2];
} MainSharedMemory;

typedef struct { long mtype; } TurnReadyRequest;

typedef struct {
    long mtype;
    int  turnNumber;
    int  newPackageRequestCount;
    int  errorOccured;
    int  finished;
} TurnChangeResponse;

typedef struct {
    long mtype;
    int  truckNumber;
    char authStringGuess[TRUCK_MAX_CAP + 1];
} SolverRequest;

typedef struct {
    long mtype;
    int  guessIsCorrect;
} SolverResponse;

#endif