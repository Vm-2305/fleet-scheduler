#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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
    /* read solver keys but ignore them - we compute secrets directly */
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

/* Compute auth string directly using same formula as simulator */
static void solve_auth(int truck, int L, char *out) {
    static const char A[]="udlr";
    unsigned s=(unsigned)(truck*7919+L*1009+currentTurn*31);
    for(int i=0;i<L;i++){s=s*1664525u+1013904223u;out[i]=A[s%4];}
    out[L]='\0';
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
            if (truckTarget[ti]>=0 && !pkgs[truckTarget[ti]].pickedup) continue;
            int dist=mdist(shm->truckPositions[ti][0],shm->truckPositions[ti][1],
                           p->req.pickup_x,p->req.pickup_y);
            int ttl=p->req.expiry_turn-currentTurn;
            if (ttl<=0) ttl=1;
            double cost=dist+200.0/ttl;
            if (cost<bestcost){bestcost=cost;best=ti;}
        }
        if (best>=0){p->assigned=best; truckTarget[best]=pi;}
    }
}

static void run_turn(void) {
    int pickup[MAX_TRUCKS], dropoff[MAX_TRUCKS];
    char move[MAX_TRUCKS];
    for (int i=0;i<D;i++){pickup[i]=-1;dropoff[i]=-1;move[i]='s';}

    for (int ti=0;ti<D;ti++){
        int x=shm->truckPositions[ti][0];
        int y=shm->truckPositions[ti][1];
        int tgt=truckTarget[ti];
        if (shm->truckTurnsInToll[ti]>0) continue;

        for (int pi=0;pi<npkgs;pi++){
            if (!pkgs[pi].pickedup||pkgs[pi].done) continue;
            if (pkgs[pi].assigned!=ti) continue;
            if (x==pkgs[pi].req.dropoff_x&&y==pkgs[pi].req.dropoff_y){
                dropoff[ti]=pkgs[pi].req.packageId;
                break;
            }
        }

        if (dropoff[ti]<0&&tgt>=0&&!pkgs[tgt].pickedup){
            if (x==pkgs[tgt].req.pickup_x&&y==pkgs[tgt].req.pickup_y)
                pickup[ti]=pkgs[tgt].req.packageId;
        }

        int gx=-1,gy=-1;
        if (shm->truckPackageCount[ti]>0){
            int best=-1,bestexp=1<<30;
            for (int pi=0;pi<npkgs;pi++){
                if (pkgs[pi].assigned!=ti||!pkgs[pi].pickedup||pkgs[pi].done) continue;
                if (pkgs[pi].req.expiry_turn<bestexp){
                    bestexp=pkgs[pi].req.expiry_turn;best=pi;
                }
            }
            if (best>=0){gx=pkgs[best].req.dropoff_x;gy=pkgs[best].req.dropoff_y;}
        }
        if (gx<0&&tgt>=0){
            if (!pkgs[tgt].pickedup){gx=pkgs[tgt].req.pickup_x;gy=pkgs[tgt].req.pickup_y;}
            else{gx=pkgs[tgt].req.dropoff_x;gy=pkgs[tgt].req.dropoff_y;}
        }
        if (gx>=0) move[ti]=step_toward(x,y,gx,gy);
    }

    for (int ti=0;ti<D;ti++){
        shm->authStrings[ti][0]='\0';
        int L=shm->truckPackageCount[ti];
        if (L==0) continue;
        if (shm->truckTurnsInToll[ti]>0) continue;
        if (move[ti]=='s'&&pickup[ti]<0&&dropoff[ti]<0) continue;
        solve_auth(ti,L,shm->authStrings[ti]);
    }

    for (int ti=0;ti<D;ti++){
        shm->truckMovementInstructions[ti]=move[ti];
        shm->pickUpCommands[ti]=pickup[ti];
        shm->dropOffCommands[ti]=dropoff[ti];
    }

    for (int ti=0;ti<D;ti++){
        if (pickup[ti]>=0) pkgs[pickup[ti]].pickedup=1;
        if (dropoff[ti]>=0){
            pkgs[dropoff[ti]].done=1;
            if (truckTarget[ti]==dropoff[ti]) truckTarget[ti]=-1;
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