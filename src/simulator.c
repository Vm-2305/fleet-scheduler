#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <time.h>
#include <signal.h>
#include "shared.h"

#define SIM_N  10
#define SIM_D   3
#define SIM_S   2
#define SIM_T  50
#define SIM_B   3
#define MAX_NEW 4

#define KEY_SHM     0x0F110001
#define KEY_TURN    0x0F110002
#define KEY_MAINQ   0x0F110003
#define KEY_SOLVERQ 0x0F110010

static const char ALPHA[]="udlr";
static int g_toll[SIM_N][SIM_N];

static void gen_secret(int truck,int len,int turn,char *out){
    unsigned s=(unsigned)(truck*7919+len*1009+turn*31);
    for(int i=0;i<len;i++){s=s*1664525u+1013904223u;out[i]=ALPHA[s%4];}
    out[len]='\0';
}

static void place_tolls(unsigned seed){
    srand(seed^0xDEAD); int n=0;
    while(n<SIM_B){
        int x=rand()%SIM_N,y=rand()%SIM_N;
        if(x==0&&y==0)continue;
        if(!g_toll[x][y]){g_toll[x][y]=(rand()%3)+1;n++;}
    }
}

static void solver_proc(int qid, int turnShmId) {
    int *turnPtr = (int*)shmat(turnShmId, NULL, 0);
    char secret[TRUCK_MAX_CAP+1];
    int tgt_truck=0, tgt_turn=1;
    for(;;){
        SolverRequest req;
        memset(&req,0,sizeof req);
        ssize_t r = msgrcv(qid,&req,sizeof(SolverRequest)-sizeof(long),0,0);
        if(r<0) continue;
        if(req.mtype==2){
            tgt_truck=req.truckNumber;
            tgt_turn=*turnPtr;
        } else if(req.mtype==3){
            int L=(int)strlen(req.authStringGuess);
            int correct=0;
            for(int dt=-2; dt<=2 && !correct; dt++){
                int t=tgt_turn+dt; if(t<1) t=1;
                gen_secret(tgt_truck,L,t,secret);
                if(strcmp(req.authStringGuess,secret)==0) correct=1;
            }
            SolverResponse rsp;
            rsp.mtype=4;
            rsp.guessIsCorrect=correct;
            msgsnd(qid,&rsp,sizeof(SolverResponse)-sizeof(long),0);
        }
    }
}

static void write_input(void){
    FILE *f=fopen("input.txt","w");
    if(!f){perror("input.txt");exit(1);}
    fprintf(f,"%d\n%d\n%d\n%d\n%d\n%d\n%d\n",
            SIM_N,SIM_D,SIM_S,SIM_T,SIM_B,KEY_SHM,KEY_MAINQ);
    for(int i=0;i<SIM_S;i++) fprintf(f,"%d\n",KEY_SOLVERQ+i);
    fclose(f);
}

static void helper(MainSharedMemory *shm,int mainQ,unsigned seed,int *turnPtr){
    srand(seed);
    PackageRequest pkgs[MAX_TOTAL_PACKAGES];
    int delivered[MAX_TOTAL_PACKAGES],npkgs=0;
    memset(delivered,0,sizeof delivered);
    int tx[SIM_D],ty[SIM_D],tpkg[SIM_D],ttoll[SIM_D];
    int tlist[SIM_D][TRUCK_MAX_CAP];
    for(int i=0;i<SIM_D;i++){
        tx[i]=ty[i]=tpkg[i]=ttoll[i]=0;
        for(int j=0;j<TRUCK_MAX_CAP;j++) tlist[i][j]=-1;
        shm->truckPositions[i][0]=shm->truckPositions[i][1]=0;
        shm->truckPackageCount[i]=shm->truckTurnsInToll[i]=0;
    }
    int tot_del=0,tot_exp=0;

    for(int turn=1;turn<=SIM_T;turn++){
        *turnPtr=turn;

        int nnew=(rand()%MAX_NEW)+1,written=0;
        for(int p=0;p<nnew&&npkgs<MAX_TOTAL_PACKAGES;p++){
            PackageRequest *pr=&pkgs[npkgs];
            pr->packageId=npkgs;
            pr->pickup_x=rand()%SIM_N; pr->pickup_y=rand()%SIM_N;
            pr->dropoff_x=rand()%SIM_N; pr->dropoff_y=rand()%SIM_N;
            pr->arrival_turn=turn; pr->expiry_turn=turn+6+rand()%10;
            shm->newPackageRequests[written]=*pr;
            shm->packageLocations[npkgs][0]=pr->pickup_x;
            shm->packageLocations[npkgs][1]=pr->pickup_y;
            written++; npkgs++;
        }

        int done=(npkgs>0);
        for(int p=0;p<npkgs;p++) if(!delivered[p]){done=0;break;}

        TurnChangeResponse tr;
        tr.mtype=2; tr.turnNumber=turn;
        tr.newPackageRequestCount=written;
        tr.errorOccured=0; tr.finished=done;
        msgsnd(mainQ,&tr,sizeof(tr)-sizeof(long),0);
        if(done){printf("[sim] done at turn %d\n",turn);fflush(stdout);break;}

        TurnReadyRequest rdy;
        msgrcv(mainQ,&rdy,sizeof(rdy)-sizeof(long),1,0);

        for(int ti=0;ti<SIM_D;ti++){
            int pid=shm->pickUpCommands[ti];
            if(pid<0||pid>=npkgs||delivered[pid]) continue;
            if(tx[ti]!=pkgs[pid].pickup_x||ty[ti]!=pkgs[pid].pickup_y) continue;
            if(tpkg[ti]>=TRUCK_MAX_CAP) continue;
            int dup=0;
            for(int c=0;c<tpkg[ti];c++) if(tlist[ti][c]==pid){dup=1;break;}
            if(dup) continue;
            tlist[ti][tpkg[ti]++]=pid;
            shm->packageLocations[pid][0]=-1;
            shm->packageLocations[pid][1]=-1;
        }

        for(int ti=0;ti<SIM_D;ti++){
            int pid=shm->dropOffCommands[ti];
            if(pid<0||pid>=npkgs) continue;
            int slot=-1;
            for(int c=0;c<tpkg[ti];c++) if(tlist[ti][c]==pid){slot=c;break;}
            if(slot<0) continue;
            if(tx[ti]!=pkgs[pid].dropoff_x||ty[ti]!=pkgs[pid].dropoff_y) continue;
            delivered[pid]=1; tot_del++;
            if(turn>pkgs[pid].expiry_turn) tot_exp++;
            tlist[ti][slot]=tlist[ti][--tpkg[ti]];
            tlist[ti][tpkg[ti]]=-1;
        }

        char sec[TRUCK_MAX_CAP+1];
        for(int ti=0;ti<SIM_D;ti++){
            if(ttoll[ti]>0){ttoll[ti]--;continue;}
            char mv=shm->truckMovementInstructions[ti];
            if(!mv||mv=='s') continue;
            if(tpkg[ti]>0){
                gen_secret(ti,tpkg[ti],turn,sec);
                if(strcmp(shm->authStrings[ti],sec)!=0) continue;
            }
            int nx=tx[ti],ny=ty[ti];
            if(mv=='r'&&nx<SIM_N-1) nx++;
            else if(mv=='l'&&nx>0) nx--;
            else if(mv=='d'&&ny<SIM_N-1) ny++;
            else if(mv=='u'&&ny>0) ny--;
            tx[ti]=nx; ty[ti]=ny;
            if(g_toll[nx][ny]) ttoll[ti]=g_toll[nx][ny];
        }

        for(int ti=0;ti<SIM_D;ti++){
            shm->truckPositions[ti][0]=tx[ti];
            shm->truckPositions[ti][1]=ty[ti];
            shm->truckPackageCount[ti]=tpkg[ti];
            shm->truckTurnsInToll[ti]=ttoll[ti];
            shm->pickUpCommands[ti]=-1;
            shm->dropOffCommands[ti]=-1;
        }
        printf("[%2d] ",turn);
        for(int ti=0;ti<SIM_D;ti++)
            printf("T%d=(%d,%d,p=%d) ",ti,tx[ti],ty[ti],tpkg[ti]);
        printf("pkgs=%d del=%d exp=%d\n",npkgs,tot_del,tot_exp);
        fflush(stdout);
    }
    printf("\n=== DONE ===\npackages=%d delivered=%d expired=%d\n",npkgs,tot_del,tot_exp);
    fflush(stdout);
}

int main(int argc,char *argv[]){
    unsigned seed=argc>1?(unsigned)atoi(argv[1]):(unsigned)time(NULL);
    printf("seed=%u\n",seed); fflush(stdout);

    {int o;
     if((o=shmget(KEY_SHM,1,0666))>=0)    shmctl(o,IPC_RMID,NULL);
     if((o=shmget(KEY_TURN,1,0666))>=0)   shmctl(o,IPC_RMID,NULL);
     if((o=msgget(KEY_MAINQ,0666))>=0)    msgctl(o,IPC_RMID,NULL);
     for(int i=0;i<SIM_S;i++)
         if((o=msgget(KEY_SOLVERQ+i,0666))>=0) msgctl(o,IPC_RMID,NULL);
    }

    int sid=shmget(KEY_SHM,sizeof(MainSharedMemory),IPC_CREAT|0666);
    if(sid<0){perror("shmget");exit(1);}
    MainSharedMemory *shm=shmat(sid,NULL,0);
    memset(shm,0,sizeof(*shm));
    for(int i=0;i<MAX_TOTAL_PACKAGES;i++){shm->packageLocations[i][0]=-1;shm->packageLocations[i][1]=-1;}
    for(int i=0;i<MAX_TRUCKS;i++){shm->pickUpCommands[i]=-1;shm->dropOffCommands[i]=-1;}

    int tsid=shmget(KEY_TURN,sizeof(int),IPC_CREAT|0666);
    if(tsid<0){perror("shmget turn");exit(1);}
    int *turnPtr=(int*)shmat(tsid,NULL,0);
    *turnPtr=1;

    int mainQ=msgget(KEY_MAINQ,IPC_CREAT|0666);
    if(mainQ<0){perror("msgget main");exit(1);}
    int solverQs[SIM_S];
    for(int i=0;i<SIM_S;i++){
        solverQs[i]=msgget(KEY_SOLVERQ+i,IPC_CREAT|0666);
        if(solverQs[i]<0){perror("msgget solver");exit(1);}
    }

    write_input();
    place_tolls(seed);

    pid_t spids[SIM_S];
    for(int i=0;i<SIM_S;i++){
        spids[i]=fork();
        if(spids[i]==0){
            solver_proc(solverQs[i], tsid);
            exit(0);
        }
    }
    usleep(100000);

    pid_t stud=fork();
    if(stud==0){execl("./solution","./solution",NULL);perror("exec");exit(1);}
    printf("student pid=%d\n",(int)stud); fflush(stdout);

    helper(shm,mainQ,seed,turnPtr);

    kill(stud,SIGTERM); waitpid(stud,NULL,0);
    for(int i=0;i<SIM_S;i++){kill(spids[i],SIGTERM);waitpid(spids[i],NULL,0);}
    shmdt(shm); shmdt(turnPtr);
    shmctl(sid,IPC_RMID,NULL); shmctl(tsid,IPC_RMID,NULL);
    msgctl(mainQ,IPC_RMID,NULL);
    for(int i=0;i<SIM_S;i++) msgctl(solverQs[i],IPC_RMID,NULL);
    return 0;
}