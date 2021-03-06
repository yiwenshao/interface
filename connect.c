#include "connect.h"
#include <stdio.h>
#include <hiredis/hiredis.h>
#include <errno.h>
#include "crc16.h"
#include <string.h>
#include <assert.h>

#define CHECK_REPLY
static char* CHIREDIS_VERSION = "1.0.4";
//the following are a list of internal function that are not intended to be used outsize this file.
static void __global_disconnect(clusterInfo* cluster);
static clusterInfo* __connect_cluster(char* ip, int port);
static clusterInfo* __clusterInfo(redisContext* localContext);
static void __test_slot(clusterInfo* mycluster);
static void __from_str_to_parseArgv(char * temp, clusterInfo* mycluster);
static void __process_clusterInfo(clusterInfo* mycluster);
static void __assign_slots(clusterInfo* mycluster);
static void __add_context_to_cluster(clusterInfo* mycluster);
static void __print_clusterInfo_parsed(clusterInfo* mycluster);
static void __remove_context_from_cluster(clusterInfo* mycluster);



static int __set_nodb(clusterInfo* cluster,const char* key,char* set_in_value);
static int __set_withdb(clusterInfo* cluster,const char* key, char* set_in_value, int dbnum,int tid);

static int __get_withdb(clusterInfo*cluster, const char* key,char*get_in_value,int dbnum,int tid);
static int __get_nodb(clusterInfo*cluster, const char* key,char* get_in_value);

static void __set_redirect(char* str);

static redisReply* __cluster_pipeline_getReply(clusterInfo *cluster,clusterPipe *mypipe);

void get_chiredis_version() {
    printf("Chiredis version = %s\n",CHIREDIS_VERSION);
}


/*
*list all the possible types of redisReply
*/
int check_reply(redisReply* reply) {
     switch(reply->type) {
     case REDIS_REPLY_STATUS:
          return 0;
          break;
     case REDIS_REPLY_ERROR:
          return -1;
          break;
     case REDIS_REPLY_INTEGER:
          return 0;
          break;
     case REDIS_REPLY_NIL:
          return 0;
          break; 
     case REDIS_REPLY_STRING:
          return 0;
          break;
     case REDIS_REPLY_ARRAY:
          return 0;
          break;
     default:
          printf("check reply error %d %s",__LINE__,__FILE__);
     }
}


clusterInfo* connectRedis(char* ip, int port){
     __connect_cluster(ip,port);
}

/*
*use the globalContext to connect to the host specified by the user,
*send "cluster nodes" command through this context,
*receive the infomation and create clusterInfo struct,
*which keeps connections to all nodes in the cluster.
*
*returns NULL if errors occur
*/
static clusterInfo* __connect_cluster(char* ip, int port){

     redisContext* localContext = redisConnect(ip,port);
	 if(localContext==NULL || localContext->err){
	     if(localContext!=NULL){
                  printf("global connection error %s %d %s\n",\
		                localContext->errstr,__LINE__,__FILE__);
	          redisFree(localContext);
		  return NULL;
	       }else{
	          printf("can not allocate global context %d %s",\
		                                  __LINE__,__FILE__);
		  return NULL;
	       }
	 }else{

	 }
	 
	clusterInfo* cluster = __clusterInfo(localContext);

	if(cluster!=NULL)
	   return cluster;
	else{
	   printf("return error in redisConnect\n");
	   return NULL;
	}
}

/*
If we want 
*/
static clusterInfo* __mallocClusterInfo() {
    clusterInfo* mycluster = (clusterInfo*)malloc(sizeof(clusterInfo));
    return mycluster;
}
/*
*This function uses the globle context to send cluster nodes command, and build a clusterInfo
*based on the string returned. It does this by calling functions from_str_to_cluster,
*process_clusterInfo,and addign_slot;
*/
static clusterInfo* __clusterInfo(redisContext* localContext) {
    redisContext *c = localContext;
    redisReply* r = (redisReply*)redisCommand(c,"cluster nodes");
    if(r == NULL) {
        printf("panic! %s %d\n",__FILE__,__LINE__);
        return NULL;
    }
    clusterInfo* mycluster = __mallocClusterInfo();

    mycluster->globalContext = localContext;

    __from_str_to_parseArgv(r->str,mycluster);
    freeReplyObject(r);
    
    __process_clusterInfo(mycluster);

    __assign_slots(mycluster);

    __add_context_to_cluster(mycluster);
    return mycluster;
}

/*
*command cluster nodes will return a str, which fall into n parts, one for each node in the 
*cluster. This function converts the strs to argv in struct clusterInfo, and set mycluster->len, which
*is the number of nodes in the cluster.
*Current version only support at most 500 masters in a cluster and it just ignores slaves.
*/
static void __from_str_to_parseArgv(char * temp, clusterInfo* mycluster) {

    char * argv[500];
    int count = 0;
    char* point;
    int copy_len = 0;

    while((point=strchr(temp,'\n'))!=NULL){
        copy_len = point-temp+1;
        argv[count] = (char*)malloc(copy_len);
        strncpy(argv[count],temp,copy_len - 1);

	if(strstr(argv[count],"slave")!=NULL){
	     free(argv[count]);
	     temp = point+1;
	     continue;
	}
        argv[count][copy_len-1] = '\0';
        count++;
        temp = point+1;
    }

    argv[count] = temp;

    int i;
    for(i = 0;i<count;i++){
        mycluster->argv[i] = argv[i];
    }

    mycluster->len = count;
}

/*
*This function should be called after from_str_to_cluster.It parses the string for each node,and 
*store the information in mycluster->parse[i]. the parse fild are pointers to  parseArgv struct,each
*node in the cluster has exactly one such struct, which contains infomation such as ip,port,slot,context..
*/
static void __process_clusterInfo(clusterInfo* mycluster){
    //determine the time of iteration
    int len = mycluster->len;
    int len_ip=0;
    int len_port=0;
    char* temp;
    char* ip_start, *port_start, *port_end,*connect_start,*slot_start,*slot_end;

    char* temp_slot_start;
    char* temp_port;

    int i=0;
    for(;i<len;i++){
        temp = ip_start = port_start = port_end = connect_start = slot_start = slot_end = temp_slot_start = temp_port = NULL;

        temp = mycluster->argv[i];

        //parse ip
        ip_start = strchr(temp,' ');
        temp = ip_start+1;
        port_start = strchr(temp,':');
        temp = port_start+1;
        port_end = strchr(temp,' ');
        ip_start++;
        len_ip = port_start - ip_start;
        mycluster->parse[i] = (parseArgv*)malloc(sizeof(parseArgv));//do not forget
        mycluster->parse[i]->ip = (char*)malloc(len_ip + 1);
        

        strncpy(mycluster->parse[i]->ip,ip_start,len_ip);
        mycluster->parse[i]->ip[len_ip]='\0';

        //parsePort
        port_start++;
        len_port = port_end - port_start;
        temp_port = (char*)malloc(len_port+1);
        strncpy(temp_port,port_start,len_port);
        temp_port[len_port]='\0';
        mycluster->parse[i]->port = atoi(temp_port);
        free(temp_port);

        connect_start = strstr(port_end,"connected");
        connect_start = strchr(connect_start,' ');

        slot_start = connect_start+1;
        slot_end = strchr(slot_start,'-');
        slot_end++;

        temp_slot_start = (char*)malloc(slot_end-slot_start);
        strncpy(temp_slot_start,slot_start,slot_end-slot_start-1);
        temp_slot_start[slot_end-slot_start-1]='\0';

        mycluster->parse[i]->start_slot = atoi(temp_slot_start);
        mycluster->parse[i]->end_slot = atoi(slot_end);

        free(temp_slot_start);

        //on default, the pipe mode doesn't open        
        mycluster->parse[i]->pipe_mode = PIPE_CLOSE;
        mycluster->parse[i]->pipe_pending = 0;
    }
}

/*
*print all the information about the cluster fro debuging.
*/
static void __print_clusterInfo_parsed(clusterInfo* mycluster){
    int len = mycluster->len;
    int i;
    for(i=0;i<len;i++){
        printf("node %d info\n",i);
        printf("original command: %s\n",mycluster->argv[i]);
        printf("ip: %s, port: %d\n",mycluster->parse[i]->ip,mycluster->parse[i]->port);
        printf("slot_start= %d, slot_end = %d\n",mycluster->parse[i]->start_slot,\
	             mycluster->parse[i]->end_slot);
    }
}


//only for testing purposes
static void __test_slot(clusterInfo* mycluster){
    int slot[] = {0,5460,5461,10922,10923,16383};
    int len = sizeof(slot) / sizeof(int);
    int i=0;
    for(i=0;i<len;i++){
        printf("slot = %d info: ip = %s, port = %d.\n",slot[i],((parseArgv*)mycluster->slot_to_host[slot[i]])->ip, ((parseArgv*)mycluster->slot_to_host[slot[i]])->port);
    }
}

/*
Assign slots to each node in the cluster. 
*/
static void __assign_slots(clusterInfo* mycluster){
    int len = mycluster->len;
    int i;
    int count = 0;
    for(i=0;i<len;i++){
        int start = mycluster->parse[i]->start_slot;
        int end = mycluster->parse[i]->end_slot;
        int j=0;
	if(sizeof(mycluster->parse[i]->slots)!=16384){
	     printf("slot != 16384 in __assign_slots\n");
	     return;
	} 
	memset(mycluster->parse[i]->slots,0,16384);
        for(j=start;j<=end;j++){
            mycluster->slot_to_host[j] = (void*)(mycluster->parse[i]);
            count++;
	    mycluster->parse[i]->slots[i]=1;
        }
    }
}

/*
*This function give each node in the cluster a context connection
*/
static void __add_context_to_cluster(clusterInfo* mycluster){
   int len = mycluster-> len;
   int i = 0;
   redisContext * tempContext;
   
   for(i=0;i<len;i++){
       tempContext = redisConnect((mycluster->parse[i])->ip,(mycluster->parse[i])->port);
       if(tempContext->err){
          printf("connection refused in __add_contect_to_cluster\n");
	  printf("refuse ip=%s, port=%d",(mycluster->parse[i])->ip,(mycluster->parse[i])->port);
	  redisFree(tempContext);
	  return;
       }else{
          (mycluster->parse[i])->context = tempContext;
       }
   }

}
//****we have finished constructing a cluster structure here*****


/*
*once we meet indirection, this function help parse the information returned
*/
static void __set_redirect(char* str){
	char *slot,*ip,*port;
	slot = strchr(str,' ');
	slot++;
	ip = strchr(slot,' ');
	ip++;
	port = strchr(ip,':');
	port++;
	//s means store
	char* s_slot, *s_ip, *s_port;
        s_slot = (char*)malloc(ip-slot);
	strncpy(s_slot,slot,ip-slot-1);
	s_slot[ip-slot-1]='\0';

	s_ip = (char*)malloc(port-ip);
	strncpy(s_ip,ip,port-ip-1);
	s_ip[port-ip-1]='\0';

	s_port = (char*)malloc(sizeof(port)+1);
	strcpy(s_port,port);

	free(s_ip);
	free(s_slot);
	free(s_port);
}

/*
*calculate the slot, find the context, and then send command
*/
static int __set_nodb(clusterInfo* cluster,const char* key,char* set_in_value){

	redisContext *c = NULL;
	int myslot;
	myslot = crc16(key,strlen(key)) & 16383;

        parseArgv* tempArgv = ((parseArgv*)(cluster->slot_to_host[myslot]));

	if(tempArgv->slots[myslot]!=1){
            //ignore the error here
	}
	if(tempArgv->context == NULL){
	    printf("context = NULL in function set\n");
	    return -1;
	}
	c = tempArgv->context;

	redisReply *r = (redisReply *)redisCommand(c, "set %s %s", key, set_in_value);

	if (r->type == REDIS_REPLY_STRING){
		printf("set should not return str ?value = %s\n", r->str);
                freeReplyObject(r);
		return -1;
	}else if(r->type == REDIS_REPLY_ERROR && !strncmp(r->str,"MOVED",5)){
		printf("set still need redirection ? %s\n", r->str);
		__set_redirect(r->str);
                freeReplyObject(r);
		return -1;
	}else if(r->type == REDIS_REPLY_STATUS){
                freeReplyObject(r);
                sprintf(set_in_value,"%s",r->str);
		return 0;
	}else{
	   printf("set error %s %d \n",__FILE__,__LINE__);
           freeReplyObject(r);
	   return -1;
	}
}

/*
*set method with the db option
*/
static int __set_withdb(clusterInfo* cluster,const char* key, char* set_in_value, int dbnum,int tid) {
        int localTid = tid%99;
	if(localTid < 0){
	   printf("local tid error in set\n");
	   return -1;
	}
	if(global_setspace[localTid].used != 0 ){
	   printf("global set space already in use\n");
	   return -1;
	}

        char* localSetKey = global_setspace[localTid].setKey;
	global_setspace[localTid].used=1;

	sprintf(localSetKey,"%d\b%s",dbnum,key);

	int re = __set_nodb(cluster,localSetKey,set_in_value);

	global_setspace[localTid].used = 0;

	return re;
}

int set(clusterInfo* cluster, const char *key,char *set_in_value,int dbnum,int tid) {
	return __set_withdb(cluster,key,set_in_value,dbnum,tid);
}


/*
*get method without use db option. here const char* is not compitable with char*
*/
static int __get_nodb(clusterInfo*cluster ,const char* key,char* get_in_value){
	if(key==NULL){
	   strcpy(get_in_value,"key is NULL");
	   return -1;
	}

	redisContext * c = NULL;
	int myslot;
	myslot = crc16(key,strlen(key)) & 16383;

	parseArgv* tempArgv = ((parseArgv*)(cluster->slot_to_host[myslot]));

	if(tempArgv->slots[myslot]!=1){
            //ignore this error currently
	}

	if(tempArgv->context == NULL){
	    //this error can not be ignored
	    printf("context = NULL in function set\n");
	    strcpy(get_in_value,"conext ==NULL");
	    return -1;
	}

	c = tempArgv->context;

	redisReply *r = (redisReply *)redisCommand(c, "get %s", key);

	if (r->type == REDIS_REPLY_STRING) {
		int len = strlen(r->str);
		strcpy(get_in_value, r->str);
		freeReplyObject(r);
		return 0;
	}else if (r->type == REDIS_REPLY_NIL) {
		strcpy(get_in_value,"nil");
		freeReplyObject(r);
		return 0;
	} else if(r->type == REDIS_REPLY_ERROR && !strncmp(r->str,"MOVED",5)){
		freeReplyObject(r);
		strcpy(get_in_value,"redirection");
		return -1;
	} else {
		printf("get return type=%d,str=%s,%d %s",r->type,\
		       r->str,__LINE__,__FILE__);
                if(r->type == REDIS_REPLY_ARRAY){
		   strcpy(get_in_value,"return bulk");  
		}else if(r->type == REDIS_REPLY_STATUS){
		   strcpy(get_in_value,"return status");
		}else if(r->type == REDIS_REPLY_INTEGER){
		      sprintf(get_in_value,"%lld",r->integer);
		}else{
		   strcpy(get_in_value,"unknown type");
		}
		freeReplyObject(r);
		return 0;
	}
}
/*
*tid can be used to  allocate global get/put space for a specific thread
*each thread only need one tid and one space
*/
static int __get_withdb(clusterInfo* cluster, const char* key,\
                            char* get_in_value,int dbnum,int tid){
        int localTid = tid%99;
	if (localTid <0) {
	    printf("local tid error\n");
	    return -1;
	}

	if(global_getspace[localTid].used == 1){
	     printf("global get space used error\n");
	     return -1;
	}
	char *localGetKey = global_getspace[localTid].getKey;
	global_getspace[localTid].used = 1;

	sprintf(localGetKey,"%d\b%s",dbnum,key);
	int re = __get_nodb(cluster,localGetKey,get_in_value);
	global_getspace[localTid].used = 0;
	return re;
}


int get(clusterInfo* cluster, const char *key, char *get_in_value,int dbnum,int tid){
      return  __get_withdb(cluster,key,get_in_value,dbnum,tid);
}

static void __remove_context_from_cluster(clusterInfo* mycluster){
   int len = mycluster-> len;
   int i = 0;
   for(i=0;i<len;i++){
      if(mycluster->parse[i]->context != NULL)      
           redisFree(mycluster->parse[i]->context);
      else printf("context == NULL in remove_context_from_cluster\n");
   }
}

static void __global_disconnect(clusterInfo *cluster){
    if(cluster->globalContext !=NULL)
        redisFree(cluster->globalContext);
}

static void __free_clusterNodes_info(clusterInfo *cluster) {
    int len = cluster->len;
    int i;
    for(i=0;i<len;i++){
       if(cluster->argv[i] != NULL)
           free(cluster->argv[i]);
       if(cluster->parse[i] != NULL) {
           if(cluster->parse[i]->ip != NULL)
               free(cluster->parse[i]->ip);
           free(cluster->parse[i]);
       }
    }
}

void disconnectDatabase(clusterInfo* cluster){
    __global_disconnect(cluster);
    __remove_context_from_cluster(cluster);
    __free_clusterNodes_info(cluster);
    free(cluster);
}


/*
*Flushdb command. 0 means the command succeed, otherwise fail, used only for cluster mode.
*
*/
int flushDb(clusterInfo* cluster){
    redisContext *c = NULL;
    if(cluster==NULL){
    	printf("error flushdb cluster == NULL\n");
    }

    int len = cluster->len;
    int i;

    for(i=0;i<len;i++){
       c = cluster->parse[i]->context;
       redisReply *r = (redisReply *)redisCommand(c, "flushdb");
       if(r->type == REDIS_REPLY_STATUS){

           printf("flushdb status = %s\n",r->str);

	}else if(r->type == REDIS_REPLY_STRING){
           printf("flush db = %s\n",r->str);
        }else{
	   break;
        }
    }
    if(i == len) return 0;
    else return -1;
}



/*
*init global space for getKey and set key, this function must be called only once before
*any operations on the redis cluster.
*/
void init_global(){
     int i;
     for(i=0;i<99;i++){
        global_getspace[i].getKey = (char*)malloc(1024);
        global_setspace[i].setKey = (char*)malloc(1024);
	global_getspace[i].used = 0;
	global_setspace[i].used = 0;
     }
}

int release_global() {
     int i;
     for(i=0;i<99;i++){
        if(global_getspace[i].getKey != NULL)
            free(global_getspace[i].getKey);
        if(global_setspace[i].setKey != NULL)
            free(global_setspace[i].setKey);
     }
}

singleClient* single_connect(int port,const char* ip){
      singleClient* sc = (singleClient*)malloc(sizeof(singleClient));
      sc->port=port;
      sc->ip = ip;
      
      redisContext* localContext = redisConnect(ip,port);
	 if(localContext==NULL || localContext->err){
	     if(localContext!=NULL){
                  printf("single global connection error %s %d %s\n",\
		                localContext->errstr,__LINE__,__FILE__);
	          redisFree(localContext);
		  return NULL;
	       }else{
	          printf("can not allocate global context %d %s",\
		                                  __LINE__,__FILE__);
		  return NULL;
	       }
	 }else{
#ifdef DEBUG	 
	     printf("succeed in global connecting\n");
#endif
             sc->singleContext=localContext; 
	     sc->pipe_count=0;
	 }
         return sc;
}

//pipeline get command
void pipe_set(singleClient*sc, char*key, char*value){
    redisAppendCommand(sc->singleContext,"SET %s %s",key,value);
    sc->pipe_count+=1;
}

//pipeline set command
void pipe_get(singleClient*sc,char*key){
   redisAppendCommand(sc->singleContext,"GET %s ",key);
   sc->pipe_count+=1;
}

//pipeline getReply
void pipe_getReply(singleClient*sc,char * revalue){
    if(sc->pipe_count==0){
        puts("pipe_count=0,should not get here!\n");
        return;
    }

    redisReply * reply;
    redisGetReply(sc->singleContext,(void**)&reply);
    switch(reply->type){
       case REDIS_REPLY_STATUS:
           sprintf(revalue,"%s",reply->str);
           //puts(reply->str);
           break;
       case REDIS_REPLY_ERROR:

          break;
       case REDIS_REPLY_INTEGER:

          break;
       case REDIS_REPLY_NIL:
          //puts("nil");
	  sprintf(revalue,"%s","nil");
          break; 
       case REDIS_REPLY_STRING:
          sprintf(revalue,"%s",reply->str);
          //puts(reply->str);
          break;
       case REDIS_REPLY_ARRAY:
          break;
       default:
          printf("check reply error %d %s",__LINE__,__FILE__);
      }
    sc->pipe_count-=1;

    freeReplyObject(reply);
}

//pipeline get all replies based on the count value
void pipe_getAllReply(singleClient*sc){
    int i=0;
    int max=sc->pipe_count;
    redisReply * reply;
    for(;i<max;i++){
     redisGetReply(sc->singleContext,(void **)&reply);
     switch(reply->type){
     case REDIS_REPLY_STATUS:
          //printf("%c\n",reply->str[0]);
          break;
     case REDIS_REPLY_ERROR:

          break;
     case REDIS_REPLY_INTEGER:

          break;
     case REDIS_REPLY_NIL:
          puts("nil");
          break; 
     case REDIS_REPLY_STRING:
          //printf("%c\n",reply->str[0]);
          break;
     case REDIS_REPLY_ARRAY:
          break;
     default:
          printf("check reply error %d %s",__LINE__,__FILE__);
      }
	sc->pipe_count-=1;
    }
    freeReplyObject(reply);
    if(sc->pipe_count != 0)
        puts("sc pipe count error\n");
}

//pipeline client
void single_disconnect(singleClient* sc){
     redisFree(sc->singleContext);
     free(sc);
     printf("disconnected!\n");
}


//start to support pipeline with redis cluster from here

/*
*This function allocate space for pipeline structure and then return a pointer to it.
*/
clusterPipe* get_pipeline(){
    clusterPipe* localPipe = (clusterPipe*)malloc(sizeof(clusterPipe));
    if(localPipe == NULL){
        printf("unable to allocate clusterPipe\n");
        return localPipe;
    }else {
        localPipe->pipe_count = 0;
        localPipe->current_count=0;
        localPipe->cur_index = 0;
        localPipe->reply_index_front = 0;
        localPipe->reply_index_end = 0;
        int i;
        for(i=0;i<MAX_PIPE_COUNT;i++){
            localPipe->send_slot[i]=-1;
            localPipe->sending_queue[i]=NULL;
            localPipe->pipe_reply_buffer[i]=NULL;
        }
    }
    return localPipe;
}

/*
*set the pipeline count
*/
int set_pipeline_count(clusterPipe* mypipe,int n) {

    if(n<0 || n>100) {
        printf("unsupported pipeline count\n");
        return -1;
    }else {
        mypipe->pipe_count = n;
       
        //then reset all
        mypipe->current_count = 0;
        mypipe->cur_index = 0;
        mypipe->reply_index_front = 0;
        mypipe->reply_index_end = 0;

        int i;
        for(i=0;i<MAX_PIPE_COUNT;i++){
            mypipe->send_slot[i]=-1;
            mypipe->sending_queue[i]=NULL;
            mypipe->pipe_reply_buffer[i]=NULL;
        }
        
        return 0;
    }
}

/*
*just to make it feel more natural to use this kind of interface.
*/
int reset_pipeline_count(clusterPipe* mypipe, int n) {
    set_pipeline_count(mypipe,n);
}



int bind_pipeline_to_cluster(clusterInfo* cluster, clusterPipe* mypipe) {

    if(cluster == NULL || mypipe == NULL) {
        printf("NULL pointer\n");
        return -1;
    }
    
    int len = cluster->len;
    for(int i=0;i<len;i++) {
        if(cluster->parse[i] == NULL){
            printf("NULL pointer %s %d",__FILE__,__LINE__);
            return -1;
        }else{
            cluster->parse[i]->pipe_mode = PIPE_OPEN;
	    cluster->parse[i]->pipe_pending = 0;
        }
    }
    mypipe->cluster = cluster;
}

/*
*base function for cluster_pipeline set and get.
*/
static int __cluster_pipeline_basecommand(clusterInfo *cluster,clusterPipe *mypipe,char *cmd,char *key,char *value){

    if(mypipe->cluster != cluster) {
        printf("haven't bind yet\n");
        return -1;
    }

    //TODO: use redisCommandAppend to send the command to buffer. and update the conresponding arrays.
    if(mypipe->current_count == mypipe->pipe_count) {
        printf("pipecount full, command rejected, please call getReply\n");
        return -1;
    }
    //Calculate the slot
    redisContext *c = NULL;
    int myslot;
    myslot = crc16(key,strlen(key)) & 16383;

    parseArgv* tempArgv = ((parseArgv*)(cluster->slot_to_host[myslot]));
    if(tempArgv == NULL) {
        printf("can't find the host for slot %d\n",myslot);
    }

    if(tempArgv->pipe_mode == PIPE_CLOSE) {
        printf("not in pipeline mode");
        return -1;
    }

    if(tempArgv->pipe_pending>100 || tempArgv->pipe_pending <0) {
        printf("invalid pending reply\n");
        return -1;
    }

    if(tempArgv->context == NULL){
        printf("context = NULL in function set\n");
        return -1;
    }
    
    c = tempArgv->context;
    if(strcmp(cmd,"set")==0)
        redisAppendCommand(c,"set %s %s",key,value);
    else if(strcmp(cmd,"get")==0)
        redisAppendCommand(c,"get %s",key);
    int current_index = mypipe->cur_index;

    mypipe->send_slot[current_index] = myslot;
    mypipe->sending_queue[current_index] = tempArgv;
    mypipe->current_count++;
    mypipe->cur_index++;
    tempArgv->pipe_pending++;
    return 0;
}


int cluster_pipeline_set(clusterInfo *cluster,clusterPipe *mypipe,char *key,char *value ) {
    return __cluster_pipeline_basecommand(cluster,mypipe,"set",key,value);
}

int cluster_pipeline_get(clusterInfo *cluster,clusterPipe *mypipe,char *key){
    return __cluster_pipeline_basecommand(cluster,mypipe,"get",key,NULL);
}

/*
*get all the replies, used internally
*/
static redisReply* __cluster_pipeline_getReply(clusterInfo *cluster,clusterPipe *mypipe){
   if(mypipe->pipe_count != mypipe->current_count){
       printf("not the right time to get all the replies\n");
       return NULL;
   }
   //TODO:
    int pipe_count = mypipe->pipe_count;
    int i=0;
    redisContext* localcontext;
    for(;i<pipe_count;i++){
        localcontext = mypipe->sending_queue[i]->context;
        redisGetReply(localcontext,(void **)&(mypipe->pipe_reply_buffer[i]));
        mypipe->sending_queue[i]->pipe_pending--;
        if(mypipe->sending_queue[i]->pipe_pending < 0) {
            printf("error %s %d\n",__FILE__,__LINE__);
            return NULL;
        }
    }
    mypipe->reply_index_end = i-1;
    return NULL;
}


int cluster_pipeline_flushBuffer(clusterInfo *cluster, clusterPipe *mypipe) {
    __cluster_pipeline_getReply(cluster,mypipe);
    return 0;
}


redisReply* cluster_pipeline_getReply(clusterInfo *cluster,clusterPipe* mypipe) {

    if(cluster == NULL || mypipe == NULL) {
        printf("NULL Pointer\n");
        return NULL;
    }

    if(mypipe->cluster != cluster) {
        printf("cluster and pipe do not match\n");
        return NULL;
    }
    
    int reply_index_front = mypipe->reply_index_front;    
    int reply_index_end = mypipe->reply_index_end;

    if(reply_index_front > reply_index_end){
        
        printf("needs more getReply or the pipeline transaction has ended\n");
        return NULL;
    }

    redisReply* reply = mypipe->pipe_reply_buffer[reply_index_front];
    mypipe->reply_index_front++;

    return reply;
}


/*
*To make sure that each pipeline transaction completes in a consistent way.
*/
bool cluster_pipeline_complete(clusterInfo *cluster,clusterPipe *mypipe) {
    //TODO: check the status to make sure that all the hosts have no pending reply, and 
    //step one: make sure that each parseArgv has exactly 0 pending reply
    int len = cluster->len;
    int i;
    for(i=0;i<len;i++) {
        assert(cluster->parse[i]->pipe_pending==0);
    }
    assert(mypipe->reply_index_front - 1 == mypipe->reply_index_end);
    assert(mypipe->pipe_count == mypipe->current_count);
    return true;
}

int release_pipeline(clusterPipe* mypipe) {
    if(mypipe != NULL)
        free(mypipe);
    return 0;
}
