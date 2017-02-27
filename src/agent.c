/*
 * agent.c
 *
 *  Created on: Dec 9, 2016
 *      Author: Saqib Ahmed
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

	/* Data access Lock */
	jrawMonitorID lock;
	JavaVM* jvm;
} GlobalAgentData;

static GlobalAgentData *gdata;
static int compiled_loaded_flag = 1;

//JVMTI Utility functions.

void fatal_error(const char * format, ...) {
	va_list ap;

	va_start(ap, format);
	(void) vfprintf(stderr, format, ap);
	(void) fflush(stderr);
	va_end(ap);
	exit(3);
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

			err = (*jvmti)->GetMethodName(jvmti, method, &name, &signature,
					&generic_ptr);
			check_jvmti_error(jvmti, err, "Get Method Name");

			err = (*jvmti)->GetMethodDeclaringClass(jvmti, method, &klass);
			check_jvmti_error(jvmti, err, "Get Declaring Class");

			err = (*jvmti)->GetClassSignature(jvmti, klass,
					&className, NULL);
			check_jvmti_error(jvmti, err, "Cannot get class signature");


			if (strstr(className, "java") == NULL
					&& strstr(className, "javax") == NULL
					&& strstr(className, "sun") == NULL && compiled_loaded_flag == 1) {
				compiled_loaded_flag++;

#ifdef COMP_FLAG
				// This is the main function call here which invokes the
				// Class_File_Load_Hook to do the analysis and, henceforth,
				// instrumentation of the code.
				err = (*jvmti)->RetransformClasses(jvmti, 1, &klass);
				check_jvmti_error(jvmti, err, "Retransform class");
#endif
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

            // Sift only the user classes from the class pool, excluding the library classes.
#ifdef COMP_FLAG
            if (name!=NULL && strstr(name, "java") == NULL && strstr(name, "sun") == NULL
					&& strstr(name, "javax") == NULL COMP_FLAG) {
#else
			if (name!=NULL && strstr(name, "java") == NULL && strstr(name, "sun") == NULL
								&& strstr(name, "javax") == NULL) {
#endif

#if DEBUG
				int nargs = 5;
            	char* args = "vdopq";
#else
            	int nargs = 3;
            	char* args = "opq";
#endif

            	// Get system class path to dump the output woker files later.
				err = (*jvmti_env)->GetSystemProperty(jvmti_env, property_1,
						&value_ptr);
				check_jvmti_error(jvmti_env, err, "Get Class Path.");
//				PATH = (void*) malloc(1000);
//				strcpy(PATH, value_ptr);
#if DEBUG
				printf("Analyzing class: %s\n", name);

	            printf("Class path is: %s\n", PATH);
	            printf("Vlaue_ptr is: %s\n", value_ptr);
#endif


				err = (*jvmti_env)->GetSystemProperty(jvmti_env, property_2,
						&value_ptr_2);
				check_jvmti_error(jvmti_env, err, "Get Class Path.");

#if DEBUG
				printf("Library path is: %s\n", value_ptr_2);
#endif

				// javab_main method of JAVAB is called here. It takes the class file as
				// a character array (C-String) in the input parameters and sets
				// the global variable of the new class data.
				javab_main(nargs, args, class_data, class_data_len);

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

#ifdef COMP_FLAG
				compiled_loaded_flag++;
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
cbVMDeath(jvmtiEnv *jvmti, JNIEnv *env)
{
        gdata->vmDead = JNI_TRUE;		// Marking VM death event for safe code execution.
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
	gdata = &data;

	gdata->jvm = jvm;

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