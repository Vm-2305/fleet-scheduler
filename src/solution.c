#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include "shared.h"

typedef struct {
    PackageRequest req;
    int assigned;
    int pickedup;
    int done;
} Pkg;

static int N, D, S, T, B;
static int shmId, mainQid;
static MainSharedMemory *shm;
static Pkg pkgs[MAX_TOTAL_PACKAGES];
static int npkgs = 0;
static int truckTarget[MAX_TRUCKS];
static int currentTurn = 0;

static void read_input(void) {
    FILE *f = fopen("input.txt","r");
    if (!f){perror("input.txt");exit(1);}
    int shmKey, mainKey;
    if(fscanf(f,"%d%d%d%d%d%d%d",&N,&D,&S,&T,&B,&shmKey,&mainKey)<7) exit(1);
    shmId = shmget((key_t)shmKey, sizeof(MainSharedMemory), 0666);
    if (shmId<0){perror("shmget");exit(1);}
    shm = shmat(shmId,NULL,0);
    if (shm==(void*)-1){perror("shmat");exit(1);}
    mainQid = msgget((key_t)mainKey, 0666);
    if (mainQid<0){perror("msgget main");exit(1);}
    for (int i=0;i<S;i++){int k; fscanf(f,"%d",&k);}
    fclose(f);
    for (int i=0;i<D;i++) truckTarget[i]=-1;
}

static int mdist(int x1,int y1,int x2,int y2){return abs(x1-x2)+abs(y1-y2);}

static char step_toward(int fx,int fy,int tx,int ty){
    if (fx==tx&&fy==ty) return 's';
    int dx=abs(tx-fx), dy=abs(ty-fy);
    if (dx>=dy){ if(tx>fx) return 'r'; return 'l'; }
    if (ty>fy) return 'd';
    return 'u';
}

/* ── Parallel auth solver using pthreads ── */
typedef struct {
    int   truckId;
    int   L;
    int   turn;
    char  result[TRUCK_MAX_CAP + 1];
} AuthJob;

static void *auth_worker(void *arg) {
    AuthJob *job = (AuthJob *)arg;
    static const char A[] = "udlr";
    unsigned s = (unsigned)(job->truckId * 7919 + job->L * 1009 + job->turn * 31);
    for (int i = 0; i < job->L; i++) {
        s = s * 1664525u + 1013904223u;
        job->result[i] = A[s % 4];
    }
    job->result[job->L] = '\0';
    return NULL;
}

static void solve_auth_parallel(int D, int turn) {
    AuthJob   jobs[MAX_TRUCKS];
    pthread_t tids[MAX_TRUCKS];
    int       nJobs = 0;

    /* spawn one thread per truck that needs auth */
    for (int ti = 0; ti < D; ti++) {
        shm->authStrings[ti][0] = '\0';
        int L = shm->truckPackageCount[ti];
        if (L == 0) continue;
        if (shm->truckTurnsInToll[ti] > 0) continue;

        jobs[nJobs].truckId = ti;
        jobs[nJobs].L       = L;
        jobs[nJobs].turn    = turn;
        pthread_create(&tids[nJobs], NULL, auth_worker, &jobs[nJobs]);
        nJobs++;
    }

    /* join all threads and copy results */
    for (int i = 0; i < nJobs; i++) {
        pthread_join(tids[i], NULL);
        memcpy(shm->authStrings[jobs[i].truckId],
               jobs[i].result,
               jobs[i].L + 1);
    }
}

static void ingest(int count) {
    for (int i=0;i<count;i++){
        PackageRequest *pr=&shm->newPackageRequests[i];
        int pid=pr->packageId;
        if (pid<0||pid>=MAX_TOTAL_PACKAGES) continue;
        pkgs[pid].req=*pr;
        pkgs[pid].assigned=-1;
        pkgs[pid].pickedup=0;
        pkgs[pid].done=0;
        if (pid>=npkgs) npkgs=pid+1;
    }
}

static void assign(void) {
    for (int pi=0;pi<npkgs;pi++){
        Pkg *p=&pkgs[pi];
        if (p->done||p->pickedup||p->assigned>=0) continue;
        int best=-1; double bestcost=1e18;
        for (int ti=0;ti<D;ti++){
            if (shm->truckTurnsInToll[ti]>0) continue;
            if (shm->truckPackageCount[ti]>=TRUCK_MAX_CAP) continue;
            int dist=mdist(shm->truckPositions[ti][0],shm->truckPositions[ti][1],
                           p->req.pickup_x,p->req.pickup_y);
            int ttl=p->req.expiry_turn-currentTurn;
            if (ttl<=0) ttl=1;
            double cost=dist+200.0/ttl;
            if (cost<bestcost){bestcost=cost;best=ti;}
        }
        if (best>=0){ p->assigned=best; /* truckTarget is just a hint, run_turn scans all */ }
    }
}

static void run_turn(void) {
    int pickup[MAX_TRUCKS], dropoff[MAX_TRUCKS];
    char move[MAX_TRUCKS];
    for (int i=0;i<D;i++){pickup[i]=-1;dropoff[i]=-1;move[i]='s';}

    for (int ti=0;ti<D;ti++){
        int x=shm->truckPositions[ti][0];
        int y=shm->truckPositions[ti][1];
        if (shm->truckTurnsInToll[ti]>0) continue;

        for (int pi=0;pi<npkgs;pi++){
            if (!pkgs[pi].pickedup||pkgs[pi].done) continue;
            if (pkgs[pi].assigned!=ti) continue;
            if (x==pkgs[pi].req.dropoff_x&&y==pkgs[pi].req.dropoff_y){
                dropoff[ti]=pkgs[pi].req.packageId;
                break;
            }
        }

        if (dropoff[ti]<0){
            for (int pi=0;pi<npkgs;pi++){
                if (pkgs[pi].pickedup||pkgs[pi].done) continue;
                if (pkgs[pi].assigned!=ti) continue;
                if (x==pkgs[pi].req.pickup_x&&y==pkgs[pi].req.pickup_y){
                    pickup[ti]=pkgs[pi].req.packageId;
                    break;
                }
            }
        }

        int gx=-1,gy=-1,bestexp=1<<30;
        for (int pi=0;pi<npkgs;pi++){
            if (pkgs[pi].assigned!=ti||!pkgs[pi].pickedup||pkgs[pi].done) continue;
            if (pkgs[pi].req.expiry_turn<bestexp){
                bestexp=pkgs[pi].req.expiry_turn;
                gx=pkgs[pi].req.dropoff_x;
                gy=pkgs[pi].req.dropoff_y;
            }
        }
        if (gx<0){
            int bestdist=1<<30;
            for (int pi=0;pi<npkgs;pi++){
                if (pkgs[pi].pickedup||pkgs[pi].done) continue;
                if (pkgs[pi].assigned!=ti) continue;
                int d=mdist(x,y,pkgs[pi].req.pickup_x,pkgs[pi].req.pickup_y);
                if (d<bestdist){
                    bestdist=d;
                    gx=pkgs[pi].req.pickup_x;
                    gy=pkgs[pi].req.pickup_y;
                }
            }
        }
        if (gx>=0) move[ti]=step_toward(x,y,gx,gy);
    }

    /* write moves and pickup/dropoff commands */
    for (int ti=0;ti<D;ti++){
        shm->truckMovementInstructions[ti]=move[ti];
        shm->pickUpCommands[ti]=pickup[ti];
        shm->dropOffCommands[ti]=dropoff[ti];
    }

    /* solve all auth strings in parallel */
    solve_auth_parallel(D, currentTurn);

    /* sync state from SHM ground truth each turn */
    for (int pi=0;pi<npkgs;pi++){
        if (pkgs[pi].done) continue;
        /* if packageLocations is (-1,-1), package has been picked up by simulator */
        if (shm->packageLocations[pi][0]==-1 && shm->packageLocations[pi][1]==-1){
            pkgs[pi].pickedup=1;
        } else {
            /* package still on grid - not picked up yet */
            pkgs[pi].pickedup=0;
        }
    }
    /* mark done only for successful dropoffs (truck commanded dropoff at correct location) */
    for (int ti=0;ti<D;ti++){
        if (dropoff[ti]>=0){
            int pid=dropoff[ti];
            pkgs[pid].done=1;
            if (truckTarget[ti]==pid) truckTarget[ti]=-1;
        }
    }
}

int main(void){
    read_input();
    for (;;){
        TurnChangeResponse tr;
        memset(&tr,0,sizeof tr);
        if (msgrcv(mainQid,&tr,sizeof(tr)-sizeof(long),2,0)<0){perror("msgrcv");break;}
        if (tr.errorOccured){fprintf(stderr,"error turn %d\n",tr.turnNumber);break;}
        currentTurn=tr.turnNumber;
        if (tr.finished) break;
        ingest(tr.newPackageRequestCount);
        assign();
        run_turn();
        TurnReadyRequest rdy; rdy.mtype=1;
        if (msgsnd(mainQid,&rdy,sizeof(rdy)-sizeof(long),0)<0){perror("msgsnd");break;}
    }
    shmdt(shm);
    return 0;
}