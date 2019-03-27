/*FreeRTOS V9.0.0 - Copyright (C) 2016 Real Time Engineers Ltd.
    All rights reserved*/

/* Standard includes. */
#include <stdint.h>
#include <stdio.h>
#include "stm32f4_discovery.h"
/* Kernel includes. */
#include "../FreeRTOS_Source/include/FreeRTOS.h"
#include "../FreeRTOS_Source/include/queue.h"
#include "../FreeRTOS_Source/include/semphr.h"
#include "../FreeRTOS_Source/include/task.h"
#include "../FreeRTOS_Source/include/timers.h"
#include "stm32f4xx.h"

typedef unsigned char BYTE;

uint32_t CREATE = 0;
uint32_t DELETE = 1;
uint32_t PERIODIC = 0;
uint32_t APERIODIC = 1;

uint32_t TIMER = -1;
uint32_t ACTIVE = -2;
uint32_t OVERDUE = -3;

uint32_t MONITOR = 1;
uint32_t LOW = 2;
uint32_t MEDIUM = 3;
uint32_t HIGH = 4;
uint32_t SCHEDULER = 5;

uint32_t ACTIVE_COUNTER=0;
uint32_t IDLE_COUNTER=0;


TimerHandle_t timer;

#define QUEUE_LENGTH 100
#define GENERATOR_WORKERS 1


static void prvSetupHardware(void);

//the task params struct
typedef struct TaskParams{
	uint32_t deadline;
	uint32_t task_type;
	uint32_t execution_time;
	uint32_t creation_time;
	uint32_t period;
	TaskHandle_t task_id;
	TimerHandle_t timer;
	uint32_t command;
} TaskParams;

//The doubly linked list for overdue and active lists
typedef struct TaskList {
	TaskParams params;
	struct TaskList *next;
	struct TaskList *prev;
} TaskList;

//The message struct to send to the scheduler
typedef struct SchedulerMessage{
	xQueueHandle queue;
	TaskParams params;
} SchedulerMessage;

xQueueHandle SchedulerQueue;

void dd_delete(TaskHandle_t xHandle);
void Watch_Deadline(TimerHandle_t xTimer);
void UserPeriodicTask(TaskParams* params);
void UserAPeriodicTask(TaskParams* params);
void dd_tcreate(TaskParams *params);
TaskList* dd_return_active_list();
TaskList* dd_return_overdue_list();
TaskList* getList(TaskParams params);
TaskList* removeFromActiveList(TaskParams params, TaskList **list);
void addToList(TaskList* entry, TaskList **list);
void DD_Scheduler_Task();
void Generator_Task();
void Monitor_Task();
void startGenerators();
static void userTaskDelay(uint32_t delay_time);

/*
 * Parameters: Task handle
 *
 * 1. Create a new TaskParams struct
 * 2. Create new queue which scheduler will respond to
 * 3. Write to the scheduler queue the created params
 * 4. Recieve the message from the scheduler
 * 5. Delete the task and the queue
 */
void dd_delete(TaskHandle_t xHandle){
	TaskParams params = {.task_id = xHandle, .command=DELETE};

	xQueueHandle TaskQueue = xQueueCreate(QUEUE_LENGTH, sizeof(int));
	vQueueAddToRegistry(TaskQueue, "messageQueue");

	SchedulerMessage message = {TaskQueue, params};
	xQueueSend(SchedulerQueue, &message, 50);

	int response_code;
	xQueueReceive(TaskQueue, &response_code, 5000);

	vQueueUnregisterQueue( TaskQueue );
	vQueueDelete( TaskQueue );
	vTaskDelete( xHandle );
}

//Watch a task and notify scheduler if deadline passed
void Watch_Deadline(TimerHandle_t xTimer){
	TaskParams params = {.timer = xTimer, .task_id=-1};
	SchedulerMessage message = {NULL, params};
	xQueueSend(SchedulerQueue, &message, 5000);
}

/* The periodic task function
 *
 * 1. Delays for the execution time
 * 2. Delete the timer
 * 3. Delete self
 */
void UserPeriodicTask(TaskParams* params){
	//delay
	for(;;){
		userTaskDelay(params->execution_time);
		xTimerDelete(params->timer, 0);
		dd_delete(params->task_id);
	}
}

/* The Aperiodic task function
 *
 * 1. Delays for the execution time
 * 2. Delete the timer
 */
void UserAPeriodicTask(TaskParams* params){
	//delay
	userTaskDelay(params->execution_time);
	xTimerDelete(params->timer, 0);
	dd_delete(params->task_id);
}

static void userTaskDelay(uint32_t delay_time){
//	uint32_t prev = xTaskGetTickCount();
//	uint32_t curr;
//	uint32_t remaining = delay_time;
//	while(remaining>0){
//		curr = xTaskGetTickCount();
//		if(curr>prev){
//			remaining--;
//			prev=curr;
//		}
//	}
	vTaskDelay(delay_time);
}

/*
 * Parameters: Task params
 *
 * 1. Create a timer to watch for the tasks deadline
 * 1. Create the task in freertos
 * 2. add timer_id, task_id to params struct
 * 3. Create a queue for the scheduler to respond to
 * 4. Send the params to the scheduler queue
 * 5. Recieve message from created queue
 * 6. Delete the queue
 */
void dd_tcreate(TaskParams *params){
	TimerHandle_t timer = xTimerCreate("watch_deadline", params->deadline, pdFALSE, (void *)0, Watch_Deadline);
	xTimerStart(timer, 0);
	TaskHandle_t xHandle = NULL;
	if (params->task_type == PERIODIC){
		xTaskCreate(UserPeriodicTask, "User", configMINIMAL_STACK_SIZE, params, LOW, &xHandle);
	}else{
		xTaskCreate(UserAPeriodicTask, "User", configMINIMAL_STACK_SIZE, params, LOW, &xHandle);
	}

	params->timer = timer;
	params->task_id = xHandle;
	params->command = CREATE;

	xQueueHandle TaskQueue = xQueueCreate(QUEUE_LENGTH, sizeof(int));
    vQueueAddToRegistry(TaskQueue, "messageQueue");

	SchedulerMessage message = {TaskQueue, *params};
	xQueueSend(SchedulerQueue, &message, 50);

	int response_code;
	xQueueReceive(TaskQueue, &response_code, 5000);

	//do something with the response code

	vQueueUnregisterQueue( TaskQueue );
	vQueueDelete( TaskQueue );
}


// Returns the active list
TaskList* dd_return_active_list(){
	TaskParams params = {.task_id = ACTIVE};
	return getList(params);
}

// Returns the overdue list
TaskList* dd_return_overdue_list(){
	TaskParams params = {.task_id = OVERDUE};
	return getList(params);
}

// Returns a generic list
TaskList* getList(TaskParams params){
	xQueueHandle TaskQueue = xQueueCreate(QUEUE_LENGTH, sizeof(TaskList*));
	vQueueAddToRegistry(TaskQueue, "messageQueue");

	SchedulerMessage message = {TaskQueue, params};
	xQueueSend(SchedulerQueue, &message, 50);

	TaskList *list;
	xQueueReceive(TaskQueue, list, 5000);

	//do something with the response code

	vQueueUnregisterQueue( TaskQueue );
	vQueueDelete( TaskQueue );
	return list;
}

/*
 * Parameters: Task params, task list
 *
 * 1. Find the item in the list that needs to be removed
 * 2. Update the priority of the next element to be medium
 * 3. Remove item from list
 */
TaskList* removeFromActiveList(TaskParams params, TaskList **list){
//	TickType_t ticks = xTaskGetTickCount();
//	uint32_t normalized_time = ticks;
	TaskList *curr = (*list);
	TaskList *prev = NULL;
	while(curr->params.task_id!=params.task_id && curr->params.timer!=params.timer){
		prev = curr;
		curr=curr->next;
	}
	if(prev==NULL){
		(*list) = curr->next;
		vTaskPrioritySet((*list)->params.task_id, MEDIUM);
//		printf("Increasing priority DELETE: {deadline: %d, execution_time: %d, creation_time: %d, current_time: %d}\n",(*list)->params.deadline, (*list)->params.execution_time,(*list)->params.creation_time, normalized_time);
		return curr;
	}
	TaskList *temp = curr;
	prev->next = curr->next;
	curr = curr->next;
	curr->prev = prev;
	vTaskPrioritySet((*list)->params.task_id, MEDIUM);
//	printf("Increasing priority DELETE: {deadline: %d, execution_time: %d, creation_time: %d, current_time: %d}\n",(*list)->params.deadline, (*list)->params.execution_time,(*list)->params.creation_time, normalized_time);
	return temp;
}

/*
 * Parameters: Task entry, task list
 *
 * 1. Find the place in the list to insert the item, sorted based on deadlines
 * 2. if item is first in list, update priority to be medium and change heads priority to be low
 * 3. Insert item into proper spot
 */
void addToList(TaskList* entry, TaskList **list){
//	TickType_t ticks = xTaskGetTickCount();
//	uint32_t normalized_time = ticks;
	//If list is empty
	if((*list) == NULL){
		(*list) = entry;
//		printf("Increasing priority HEAD: {deadline: %d, execution_time: %d, creation_time: %d, current_time: %d}\n",(*list)->params.deadline, (*list)->params.execution_time,(*list)->params.creation_time, normalized_time);
		vTaskPrioritySet(entry->params.task_id, MEDIUM);
		return;
	}
	TaskList *curr = (*list)->next;
	TaskList *prev = (*list);
	//if head is needed to be switched
	if((prev->params.creation_time+prev->params.deadline)>(entry->params.creation_time+entry->params.deadline)){
//		printf("Lowering priority: {deadline: %d, execution_time: %d, creation_time: %d, current_time: %d}\n",(*list)->params.deadline, (*list)->params.execution_time,(*list)->params.creation_time, normalized_time);
		vTaskPrioritySet((*list)->params.task_id, LOW);
		(*list) = entry;
		(*list)->next = prev;
		prev->prev = (*list);
		prev->next = NULL;
		vTaskPrioritySet((*list)->params.task_id, MEDIUM);
//		printf("Increasing priority: {deadline: %d, execution_time: %d, creation_time: %d, current_time: %d}\n",(*list)->params.deadline, (*list)->params.execution_time,(*list)->params.creation_time, normalized_time);
		return;
	}
	while(curr!=NULL && entry->params.deadline > curr->params.deadline){
		prev = curr;
		curr=curr->next;
	}
	//if inserting at end
	if(curr==NULL){
		prev->next = entry;
		entry->prev = prev;
		return;
	}
	//Inserting middle
	prev->next = entry;
	entry->next = curr;
	entry->prev = prev;
	curr->prev = entry;
}

/*
 * 1. If a timer has gone off, remove from active list, delete and add to overdue list
 * 2. If a get list request, respond to queue
 * 3. If delete request, remove from active list and respond
 * 4. else create a new task and insert into list
 */
void DD_Scheduler_Task(){
	SchedulerMessage message;
	TaskList *active = NULL;
	TaskList *overdue = NULL;
	for(;;){
		if (xQueueReceive(SchedulerQueue, &message, 50000)){
			//timer message
			if(message.params.task_id==TIMER){
				TaskList *entry = removeFromActiveList(message.params, &active);
				vTaskDelete(entry->params.task_id);
				addToList(entry, &overdue);
			//dd_return_active_list
			}else if(message.params.task_id==ACTIVE){
				xQueueSend(message.queue, active, 50);
			//dd_return_overdue_list
			}else if(message.params.task_id==OVERDUE){
				xQueueSend(message.queue, overdue, 50);
			}else{
				if(message.params.command==DELETE){
					TaskList *entry = removeFromActiveList(message.params, &active);
					free(entry);
					xQueueSend(message.queue, 1, 50);
				}else{
					TaskList *entry = (TaskList*)malloc(sizeof(TaskList));
					entry->params = message.params;
					TickType_t ticks = xTaskGetTickCount();
					entry->params.creation_time = ticks;
					addToList(entry, &active);
					xQueueSend(message.queue, 1, 50);
				}
			}
		}
	}
}

void Generator_Task1(){
	TaskParams params = {.period = 2000, .deadline = 1000, .execution_time=500, .task_type=PERIODIC};
	for(;;){
//		printf("Generating new task1\n");
		dd_tcreate(&params);
		vTaskDelay(params.period);
	}
}

void Generator_Task2(){
	TaskParams params = {.period = 2000, .deadline = 600, .execution_time=250, .task_type=PERIODIC};
	vTaskDelay(250);
	for(;;){
//		printf("Generating new task2\n");
		dd_tcreate(&params);
		vTaskDelay(params.period);
	}
}

void Generator_Task3(){
	TaskParams params = {.period = 2000, .deadline = 200, .execution_time=100, .task_type=PERIODIC};
	vTaskDelay(4100);
	for(;;){
//		printf("Generating new task3\n");
		dd_tcreate(&params);
		vTaskDelay(params.period);
	}
}

// Used to check the processor utilization
void Processor_Delay(TimerHandle_t xTimer){
	TaskList* list = dd_return_active_list();
	if(list!=NULL){
		ACTIVE_COUNTER++;
	}else{
		IDLE_COUNTER++;
	}
}


//Monitors the
void Monitor_Task(){
	for(;;){
		printf("Processor utilization: %d",ACTIVE_COUNTER/IDLE_COUNTER);
		TaskList* active = dd_return_active_list();
		TaskList* overdue = dd_return_overdue_list();
		printf("Active list highest priority: {deadline: %d, execution_time: %d, creation_time: %d}\n",active->params.deadline, active->params.execution_time,active->params.creation_time);
		printf("overdue list head: {deadline: %d, execution_time: %d, creation_time: %d}\n",overdue->params.deadline, overdue->params.execution_time, overdue->params.creation_time);

	}
}

int main(void) {
  prvSetupHardware();

  //start the timer to monitor
  TimerHandle_t timer = xTimerCreate("watch_processor", 5, pdTRUE, (void *)0, Processor_Delay);
  xTimerStart(timer, 0);

  // Initialize the four queues needed to communicate
  SchedulerQueue = xQueueCreate(QUEUE_LENGTH, sizeof(SchedulerMessage));

  // Add the queues to the registry
  vQueueAddToRegistry(SchedulerQueue, "SchedulerQueue");

  xTaskCreate(Generator_Task1, "Generator1", configMINIMAL_STACK_SIZE, NULL, HIGH, NULL);
  xTaskCreate(Generator_Task2, "Generator2", configMINIMAL_STACK_SIZE, NULL, HIGH, NULL);
  xTaskCreate(Generator_Task3, "Generator3", configMINIMAL_STACK_SIZE, NULL, HIGH, NULL);


  xTaskCreate(DD_Scheduler_Task, "DDScheduler", configMINIMAL_STACK_SIZE, NULL, SCHEDULER, NULL);
  xTaskCreate(Monitor_Task, "MonitorTask", configMINIMAL_STACK_SIZE, NULL, MONITOR, NULL);

  // Start the scheduler
  vTaskStartScheduler();

  return 0;
}

void vApplicationMallocFailedHook(void) {
  /* The malloc failed hook is enabled by setting
  configUSE_MALLOC_FAILED_HOOK to 1 in FreeRTOSConfig.h.

  Called if a call to pvPortMalloc() fails because there is insufficient
  free memory available in the FreeRTOS heap.  pvPortMalloc() is called
  internally by FreeRTOS API functions that create tasks, queues, software
  timers, and semaphores.  The size of the FreeRTOS heap is set by the
  configTOTAL_HEAP_SIZE configuration constant in FreeRTOSConfig.h. */
  for (;;)
    ;
}
/*-----------------------------------------------------------*/

void vApplicationStackOverflowHook(xTaskHandle pxTask,
                                   signed char *pcTaskName) {
  (void)pcTaskName;
  (void)pxTask;

  /* Run time stack overflow checking is performed if
  configconfigCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2.  This hook
  function is called if a stack overflow is detected.  pxCurrentTCB can be
  inspected in the debugger if the task name passed into this function is
  corrupt. */
  for (;;)
    ;
}
/*-----------------------------------------------------------*/

void vApplicationIdleHook(void) {
//  volatile size_t xFreeStackSpace;
//
//  /* The idle task hook is enabled by setting configUSE_IDLE_HOOK to 1 in
//  FreeRTOSConfig.h.
//
//  This function is called on each cycle of the idle task.  In this case it
//  does nothing useful, other than report the amount of FreeRTOS heap that
//  remains unallocated. */
////  xFreeStackSpace = xPortGetFreeHeapSize();
//
//  if (xFreeStackSpace > 100) {
//    /* By now, the kernel has allocated everything it is going to, so
//    if there is a lot of heap remaining unallocated then
//    the value of configTOTAL_HEAP_SIZE in FreeRTOSConfig.h can be
//    reduced accordingly. */
//  }
}
/*-----------------------------------------------------------*/

static void prvSetupHardware(void) {
  /* Ensure all priority bits are assigned as preemption priority bits.
  http://www.freertos.org/RTOS-Cortex-M3-M4.html */
  NVIC_SetPriorityGrouping(0);

  /* TODO: Setup the clocks, etc. here, if they were not configured before
  main() was called. */
}
