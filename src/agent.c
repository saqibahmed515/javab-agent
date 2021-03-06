/*
 * Copyright (c) 2017, Al-Khawarizmi Institute of Computer Sciences
 * and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact KICS, G.T. Road UET, Lahore, Pakistan or visit www.kics.edu.pk
 * if you need additional information or have any questions.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include "class.h"
#include "jvmti.h"
#include "jni.h"

/* Global agent data structure */

typedef struct {
	/* JVMTI Environment */
	jvmtiEnv *jvmti;
	JNIEnv * jni;
	jboolean vm_is_started;
	jboolean vmDead;
	char ** class_list;
 	int list_index;
	char ** par_class_list;
	int par_index;
    
	/* Data access Lock */
	jrawMonitorID lock;
	JavaVM* jvm;
} GlobalAgentData;

#define F_LIST_LENGTH 5
static GlobalAgentData *gdata;
static int compiled_loaded_flag = 1;
static char *f_list[F_LIST_LENGTH];
//JVMTI Utility functions.

void fatal_error(const char * format, ...) {
	va_list ap;

	va_start(ap, format);
	(void) vfprintf(stderr, format, ap);
	(void) fflush(stderr);
	va_end(ap);
	exit(3);
}

// 1 = if not present in list
// 0 = if present and should be filtered
int filter_check(char * a){

	int N,result=1;
	for(N=0;N<F_LIST_LENGTH;N++){
		if(strstr(a,f_list[N]) != NULL)
			result=0;
	}
	return result;
}

void check_jvmti_error(jvmtiEnv *jvmti, jvmtiError errnum, const char *str) {
	if (errnum != JVMTI_ERROR_NONE) {
		char *errnum_str;

		errnum_str = NULL;
		(void) (*jvmti)->GetErrorName(jvmti, errnum, &errnum_str);

		fatal_error("ERROR: JVMTI: %d(%s): %s\n", errnum,
				(errnum_str == NULL ? "Unknown" : errnum_str),
				(str == NULL ? "" : str));
	}
}

// Enter a critical section by doing a JVMTI Raw Monitor Enter
static void
enterCriticalSection(jvmtiEnv *jvmti)
{
    jvmtiError error;

    error = (*jvmti)->RawMonitorEnter(jvmti, gdata->lock);
    check_jvmti_error(jvmti, error, "Cannot enter with raw monitor");
}

// Exit a critical section by doing a JVMTI Raw Monitor Exit
static void
exitCriticalSection(jvmtiEnv *jvmti)
{
    jvmtiError error;

    error = (*jvmti)->RawMonitorExit(jvmti, gdata->lock);
    check_jvmti_error(jvmti, error, "Cannot exit with raw monitor");
}


// Callback for JIT invocation. This function is invoked whenever JIT compiles
// something. JIT invokes this callback at the end of its processing.
void JNICALL
compiled_method_load(jvmtiEnv *jvmti, jmethodID method, jint code_size,
		const void* code_addr, jint map_length, const jvmtiAddrLocationMap* map,
		const void* compile_info) {

	enterCriticalSection(jvmti);
	{
		if (!gdata->vmDead) {
			jvmtiError err;
			jclass klass;

			char* name = NULL;
			char* className = NULL;
			char* signature = NULL;
			char* generic_ptr = NULL;

			err = (*jvmti)->GetMethodName(jvmti, method, &name, &signature,	&generic_ptr);
			check_jvmti_error(jvmti, err, "Get Method Name");

			err = (*jvmti)->GetMethodDeclaringClass(jvmti, method, &klass);
			check_jvmti_error(jvmti, err, "Get Declaring Class");

			err = (*jvmti)->GetClassSignature(jvmti, klass,	&className, NULL);
			check_jvmti_error(jvmti, err, "Cannot get class signature");

#if DEBUG>1 
  			printf("Our JVMTI- compiled_method_load for class : %s and method %s is hot \n",className, name);   
#endif
			if (filter_check(className) && compiled_loaded_flag == 1) {
	 			int parF = 0, b=0;
				int par_idx= gdata->par_index;
				for(b=0;b<par_idx;b++){
					if(strstr(className, gdata->par_class_list[b]) != NULL){
#if DEBUG>1
					printf("par_class_list=%s matched some part class name %s method name %s\n",
					gdata->par_class_list[b],className, name);
#endif
					parF=1;					
					break;
					}			
				}
	 			
				if(parF ==0){

			// checking class name in list, if the list is not empty

    				int index = gdata->list_index;			
				int file_found = 0, a=0;
				
				for(a=0;a<index;a++){
#if DEBUG>1
				 	printf("class_list: %s\n",gdata->class_list[a]);
#endif
					if(strstr(gdata->class_list[a], className) != NULL && 
						strstr(gdata->class_list[a], name) != NULL){
					file_found=1;					
					break;
					}			
				}
 			

				if(file_found == 0){
					strcpy(gdata->class_list[index], className);	
					strcat(gdata->class_list[index], name);	
#if DEBUG>1
					printf("the entry saved in class_list %s\n",gdata->class_list[index]);
#endif
					gdata->list_index++;
					compiled_loaded_flag++;					
		
#if DEBUG
         			printf("Our JVMTI- compiled_method_load for class : %s and method %s Selected for Analysis \n",className, name);    
#endif
				
#ifdef COMP_FLAG
				// This is the main function call here which invokes the
				// Class_File_Load_Hook to do the analysis and, henceforth,
				// instrumentation of the code.
				err = (*jvmti)->RetransformClasses(jvmti, 1, &klass);
				check_jvmti_error(jvmti, err, "Retransform class");
#endif
				}
			}

			// Freeing the allocated memory.
			if (name != NULL) {
				err = (*jvmti)->Deallocate(jvmti, (unsigned char*) name);
				check_jvmti_error(jvmti, err, "deallocate name");
			}
			if (signature != NULL) {
				err = (*jvmti)->Deallocate(jvmti, (unsigned char*) signature);
				check_jvmti_error(jvmti, err, "deallocate signature");
			}
			if (generic_ptr != NULL) {
				err = (*jvmti)->Deallocate(jvmti, (unsigned char*) generic_ptr);
				check_jvmti_error(jvmti, err, "deallocate generic_ptr");
			}
		  }			
		}
	}
	exitCriticalSection(jvmti);
}

// Implements the callback for class file loading event. This function can invoke
// during all phases of JVM execution cycle because of dynamic class loading.
// This function calls JAVAB and does the actual analysis and instrumentation.
void JNICALL
Class_File_Load_Hook(jvmtiEnv *jvmti_env, JNIEnv* jni_env,
		jclass class_being_redefined, jobject loader, const char* name,
		jobject protection_domain, jint class_data_len,
		const unsigned char* class_data, jint* new_class_data_len,
		unsigned char** new_class_data) {

	enterCriticalSection(jvmti_env);
	{
		if (!gdata->vmDead) {
			jvmtiError err;
			unsigned char* jvmti_space = NULL;
			const char* property_1 = "java.class.path";
			const char* property_2 = "java.library.path";
			char* value_ptr = NULL;
			char* value_ptr_2 = NULL;
			jvmtiPhase phase;
			*new_class_data_len = 0;
            		*new_class_data     = NULL;

            		err = (*jvmti_env)->GetPhase(jvmti_env, &phase);
            		check_jvmti_error(jvmti_env, err, "Get Phase.");
#if DEBUG>1
			printf("Loading class: %s\n", name);
#endif
            // Shift only the user classes from the class pool, excluding the library classes.
#ifdef COMP_FLAG
            		if (name!=NULL && compiled_loaded_flag == 2) {
#else
			if (name!=NULL ){
#endif
				if( filter_check(name) && 
				check_valid_CP(class_data, class_data_len) ){

#if DEBUG
				int nargs = 4;
        	    		char* args = "vopq";
#else
		            	int nargs = 3;
        		    	char* args = "opq";
#endif
		
            	// Get system class path to dump the output woker files later.
				err = (*jvmti_env)->GetSystemProperty(jvmti_env, property_1,
						&value_ptr);
				check_jvmti_error(jvmti_env, err, "Get Class Path.");
				err = (*jvmti_env)->GetSystemProperty(jvmti_env, property_2,
						&value_ptr_2);
				check_jvmti_error(jvmti_env, err, "Get Class Path.");

#if DEBUG
				printf("Analyzing class: %s\n", name);
#endif

				// javab_main method of JAVAB is called here. It takes the class file as
				// a character array (C-String) in the input parameters and sets

				// the global variable of the new class data.

				int prev_index= gdata->list_index;
				char* cmplt_entry;
				char* me;
				cmplt_entry = strdup(gdata->class_list[prev_index-1]);
				strtok_r(cmplt_entry, ";", &me );
		
				javab_main(nargs, args, class_data, class_data_len, me);

				//Adding workers name into the list
				int par_idx = gdata->par_index;
				if(num_workers !=0) {
					int a=0, f=1;
					for(a=0;a < par_idx ;a++){
						//find name in list
						if(strstr(gdata->par_class_list[a], name) == 0){ 
							f=0;
							break;
						}	
					}
					
					if(f==1){
						strcpy(gdata->par_class_list[par_idx], name);
						gdata->par_index++;				
					}					
				}

				err = (*jvmti_env)->Allocate(jvmti_env, (jlong) global_pos,
						&jvmti_space);
				check_jvmti_error(jvmti_env, err, "Allocate new class Buffer.");

				(void) memcpy((void*) jvmti_space, (void*) new_class_ptr,
						(int) global_pos);

				// These pointer assignments stand for the actual instrumentation of the code.
				*new_class_data_len = (jint) global_pos;
				*new_class_data = jvmti_space;



				// Freeing the allocated memory to avoid any sort of memory leakages.
				if (new_class_ptr != NULL) {
					(void) free((void*) new_class_ptr);
					new_class_ptr = NULL;
				}
				if (PATH != NULL) {
					(void) free((void*) PATH);
					PATH = NULL;
				}
				if (value_ptr != NULL) {
					err = (*jvmti_env)->Deallocate(jvmti_env,
							(unsigned char*) value_ptr);
					check_jvmti_error(jvmti_env, err, "deallocate generic_ptr");
				}
				if (value_ptr_2 != NULL) {
					err = (*jvmti_env)->Deallocate(jvmti_env,
							(unsigned char*) value_ptr_2);
					check_jvmti_error(jvmti_env, err, "deallocate generic_ptr");
				}

#if DEBUG>1
				// The original class file is printed in the hex format.
				printf("Size of the class is: %d\n", class_data_len);
				for (int i = 0; i < class_data_len; i += 4) {
					if (i % 16 == 0)
					printf("\n");

					printf("%02x%02x  %02x%02x  ", class_data[i],
							class_data[i + 1], class_data[i + 2],
							class_data[i + 3]);
				}
				printf("\n");
//				system("javap -c -v ~~debug_class_1753~~.class Javab_Test_Worker_main_0");
#endif
				}
#ifdef COMP_FLAG
				compiled_loaded_flag=1;
#endif
			}
			

		}
	}exitCriticalSection(jvmti_env);
}

// This callback is invoked at the start of LIVE phase of JVM.
// Refer to JVMTI documentation for more detail on JVM phases.
static void JNICALL
cbVMInit(jvmtiEnv *jvmti, JNIEnv *env, jthread thread)
{
	/* Deliberately empty*/
}

// Callback function for JVM death. This is the last function executed
// in an agent.
static void JNICALL
cbVMDeath(jvmtiEnv *jvmti, JNIEnv *env) {
	int i = 0;
	for (i = 0; i < num_workers; i++) {
		char *buffer = (char *) make_mem(strlen(worker_array[i]) + 6u);
		sprintf(buffer, "rm -f %s", worker_array[i]);
		system(buffer);
		if (worker_array[i] != NULL)
			free(worker_array[i]);
		worker_array[i] = NULL;

		if (buffer != NULL) {
			free(buffer);
			buffer = NULL;
		}
	}

	if(worker_array!= NULL)
		free(worker_array);
	worker_array=NULL;

	gdata->vmDead = JNI_TRUE;// Marking VM death event for safe code execution.
#if LOG
	fclose(stdout);
	fclose(stderr);
#endif
}


// Conditionally compiled thread start callback. This function simply prints identifier
// of any thread that gets spawned in JVM. It is invoked every time a thread is spawned.
#if DEBUG_THREADS
static void JNICALL callbackThreadStart(jvmtiEnv *jvmti, JNIEnv* jni_env,
		jthread thread) {

	jvmtiError err1, err2;
	jvmtiThreadInfo info1;

	// Make sure the stack variables are garbage free

	(void) memset(&info1, 0, sizeof(info1));
	err1 = (*jvmti)->GetThreadInfo(jvmti, thread, &info1);
	if (err1 == JVMTI_ERROR_NONE)
		printf("Running Thread: %s, Priority: %d, context class loader:%s\n",
				info1.name, info1.priority,
				(info1.context_class_loader == NULL ? ": NULL" : "Not Null"));

	// Every string allocated by JVMTI needs to be freed
	err2 = (*jvmti)->Deallocate(jvmti, (unsigned char*) info1.name);
	check_jvmti_error(jvmti, err2, "Dealocate Thread Name");
}
#endif


// Executes on agent start. This function executes in the Start Phase of JVM.
// It does some basic initializations including (but not limited to) capability
// enabling, call back method registration, some sanity checks and assertions.
JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM *jvm, char *options, void *reserved) {
	static jvmtiEnv *jvmti = NULL;
	static jvmtiCapabilities capa;
	jvmtiError error;
	jvmtiEventCallbacks callbacks;
	static GlobalAgentData data;
	jint res;

	memset((void*) &data, 0, sizeof(data));
	data.list_index =0 ;	
	data.class_list = make_mem(3000 * sizeof(char *));
			
	int a;	
	for(a=0;a< 3000;a++){
		char *list_entry = (char *) make_mem(150* sizeof(char));
		data.class_list[a]= list_entry;	
	}

	data.par_index = 0;	
	data.par_class_list = make_mem(30* sizeof(char *));
		
	for(a=0;a< 30;a++){
		char *ent = (char *) make_mem(150* sizeof(char));
		data.par_class_list[a]= ent;	
	}
	
// filter stuff with names
	char * f0 = "java";
	char * f1 = "jdk";
	char * f2 = "javax";
	char * f3 = "sun";
	char * f4 = "org/eclipse/jdt/internal";
	f_list[0] = f0;	
	f_list[1] = f1;	
	f_list[2] = f2;	
	f_list[3] = f3;
	f_list[4] = f4;	

	gdata = &data;
 
	gdata->jvm = jvm;
#if LOG
	char log_name[32]; 
        snprintf(log_name, sizeof(char) * 32, "/tmp/agentLog%i.txt", (int)getpid());
	
	freopen(log_name, "a+", stdout);
	freopen(log_name, "a+", stderr);
#endif
	res = (*jvm)->GetEnv(jvm, (void **) &jvmti, JVMTI_VERSION_1_0);

	if (res != JNI_OK || jvmti == NULL) {
		/* This means that the VM was unable to obtain this version of the
		 *   JVMTI interface, this is a fatal error.
		 */
		printf("ERROR: Unable to access JVMTI Version 1 (0x%x),"
				" is your J2SE a 1.5 or newer version?"
				" JNIEnv's GetEnv() returned %d\n", JVMTI_VERSION_1, res);

	}

	// Setting up capability set for this agent.

	memset(&capa, 0, sizeof(jvmtiCapabilities));

	capa.can_generate_compiled_method_load_events = 1;
#if DEBUG_THREADS
	capa.can_signal_thread = 1;
#endif
	capa.can_retransform_classes = 1;
	capa.can_generate_all_class_hook_events = 1;

	error = (*jvmti)->AddCapabilities(jvmti, &capa);
	check_jvmti_error(jvmti, error, "Add Capabilities");

	// Enabling JVMTI events.

#if DEBUG_THREADS
	error = (*jvmti)->SetEventNotificationMode (jvmti, JVMTI_ENABLE,
			JVMTI_EVENT_THREAD_START, (jthread)NULL);
	check_jvmti_error(jvmti, error, "Set Event for Thread Start");
#endif

    error = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE,
                          JVMTI_EVENT_VM_INIT, (jthread)NULL);
    check_jvmti_error(jvmti, error, "Cannot set event notification");

    error = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE,
                          JVMTI_EVENT_VM_DEATH, (jthread)NULL);
    check_jvmti_error(jvmti, error, "Cannot set event notification");

	error = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE,
			JVMTI_EVENT_COMPILED_METHOD_LOAD, NULL);
	check_jvmti_error(jvmti, error, "Set Event for Compiled Method Load");

	error = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE,
			JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL);
	check_jvmti_error(jvmti, error, "Set Event for Compiled Method Load");


	// Registering Call Back functions

	memset(&callbacks, 0, sizeof(callbacks));

#if DEBUG_THREADS
	callbacks.ThreadStart = &callbackThreadStart;         /* JVMTI_EVENT_THREAD_START */
#endif

	callbacks.CompiledMethodLoad = &compiled_method_load; /* JVMTI_COMPILED_METHOD_LOAD */
    	callbacks.VMInit = &cbVMInit;                         /* JVMTI_EVENT_VM_INIT */	
 	callbacks.VMDeath = &cbVMDeath;                       /* JVMTI_EVENT_VM_DEATH */
	callbacks.ClassFileLoadHook = &Class_File_Load_Hook;  /* JVMTI_EVENT_CLASS_FILE_LOAD_HOOK */

	error = (*jvmti)->SetEventCallbacks(jvmti, &callbacks,
			(jint) sizeof(callbacks));
	check_jvmti_error(jvmti, error, "Set Event for CallBacks");

	// create coordination monitor
	error = (*jvmti)->CreateRawMonitor(jvmti, "agent lock", &(gdata->lock));
	check_jvmti_error(jvmti, error, "Create raw Monitor");

	
	return JNI_OK;
}
