// ECE 455 Lab project
// Mustafa Wasif V00890184
// Spencer Davis V00759537


// Standard includes.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "stm32f4_discovery.h"
#include <inttypes.h>

// Kernel includes.
#include "stm32f4xx.h"
#include "../FreeRTOS_Source/include/FreeRTOS.h"
#include "../FreeRTOS_Source/include/queue.h"
#include "../FreeRTOS_Source/include/semphr.h"
#include "../FreeRTOS_Source/include/task.h"
#include "../FreeRTOS_Source/include/timers.h"

// Struct for packaging messages on DDS_Queue.
struct DDS_Message
{
	uint16_t message_type;
	uint16_t ud_task;
	TaskHandle_t t_handle;
	uint16_t task_type;
	uint32_t task_id;
	TickType_t release_time;
	TickType_t absolute_deadline;
	TickType_t completion_time;
};

// Struct for packaging message on Monitor_Queue.
struct Monitor_Message
{
	uint16_t message_type;
	uint16_t ud_task;
	TickType_t time;
	uint16_t active_count;
	uint16_t completed_count;
	uint16_t overdue_count;
};

// Struct representing a DD task. One node in a DD task list.
struct DD_Task
{
	uint16_t ud_task;
	TaskHandle_t t_handle;
	uint16_t task_type;
	uint32_t task_id;
	uint32_t release_time;
	uint32_t absolute_deadline;
	uint32_t completion_time;
	bool started; // Used to determine whether to call xTaskCreate.
	struct DD_Task *next;
};

// Task functions.
static void DD_Task_Generator_Task(void *pvParameters);
static void Monitor_Task(void *pvParameters);
static void DDS_Task(void *pvParameters);
static void Print_Event_Time(uint16_t event_type, uint16_t ud_task, TickType_t time);
static void Print_DD_Task_List_Sizes(uint16_t active_count, uint16_t completed_count, uint16_t overdue_count);
static void Release_DD_Task(uint16_t ud_task, TaskHandle_t t_handle, uint16_t task_type, uint32_t task_id, TickType_t release_time, TickType_t absolute_deadline);
static void Complete_DD_Task(TickType_t completion_time);
static void Request_DD_Task_Counts();
static void Enlist_Task_By_Deadline(struct DD_Task** list_head, struct DD_Task* dd_task);
static void Enlist_Task(struct DD_Task** list_head, struct DD_Task* task);
static struct DD_Task* Delist_Task(struct DD_Task** list_head);
static void Schedule_Next_Task(struct DD_Task* list_head);
static void UD_Task_1( void *pvParameters );
static void UD_Task_2( void *pvParameters );
static void UD_Task_3( void *pvParameters );

// Queue handles.
xQueueHandle xTask_Generator_Queue = 0;
xQueueHandle xDDS_Queue = 0;
xQueueHandle xMonitor_Queue = 0;

// Timer handles.
xTimerHandle xUD_Task_1_Release_Timer = 0;
xTimerHandle xUD_Task_2_Release_Timer = 0;
xTimerHandle xUD_Task_3_Release_Timer = 0;
xTimerHandle xMonitor_Request_Timer = 0;

// Timer callback functions.
static void Enqueue_UD_Task_1_Release_Timer_Flag(xTimerHandle pxTimer);
static void Enqueue_UD_Task_2_Release_Timer_Flag(xTimerHandle pxTimer);
static void Enqueue_UD_Task_3_Release_Timer_Flag(xTimerHandle pxTimer);
static void Enqueue_Monitor_Request(xTimerHandle pxTimer);
static void Enqueue_Monitor_Request(xTimerHandle pxTimer);

// Defines.
#define mainQUEUE_LENGTH 100
#define QUEUE_WAIT_TIME 100
#define UD_TASK_1 1
#define UD_TASK_2 2
#define UD_TASK_3 3
#define UD_TASK_1_PERIOD 500
#define UD_TASK_2_PERIOD 500
#define UD_TASK_3_PERIOD 500
#define UD_TASK_1_EXECUTION_TIME 100
#define UD_TASK_2_EXECUTION_TIME 200
#define UD_TASK_3_EXECUTION_TIME 200
#define TASK_RELEASE 0
#define TASK_COMPLETE 1
#define TASK_OVERDUE 2
#define MONITOR_REQUEST_TASK_COUNTS 3
#define MONITOR_REQUEST_TIMER_EXPIRED 4
#define PERIODIC 0
#define APERIODIC 1
#define MONITOR_REQUEST_PERIOD 250

// Globals for output control.
bool print_release_times = false;
bool print_completion_times = false;
bool print_overdue_times = false;
bool print_dd_task_list_sizes = true;

/*-----------------------------------------------------------*/

int main(void)
{
	// Do we need this?:
	// NVIC_SetPriorityGrouping( 0 );

	// Create tasks and priorities.
	xTaskCreate( DD_Task_Generator_Task, "DD_Task_Generator_Task", configMINIMAL_STACK_SIZE, NULL, 5, NULL);
	xTaskCreate( DDS_Task, "DDS_Task", configMINIMAL_STACK_SIZE, NULL, 4, NULL);
	xTaskCreate( Monitor_Task, "Monitor_Task", configMINIMAL_STACK_SIZE, NULL, 3, NULL);

	// Create queues.
	xTask_Generator_Queue = xQueueCreate(mainQUEUE_LENGTH, sizeof( uint16_t ) );
	xDDS_Queue = xQueueCreate( mainQUEUE_LENGTH, sizeof( struct DDS_Message ) );
	xMonitor_Queue = xQueueCreate( mainQUEUE_LENGTH, sizeof( struct Monitor_Message ));

	// Add queues to the registry, for the benefit of kernel aware debugging.
	vQueueAddToRegistry( xTask_Generator_Queue, "Task_Generator_Queue" );
	vQueueAddToRegistry( xDDS_Queue, "DDS_Queue" );
	vQueueAddToRegistry( xMonitor_Queue, "Monitor_Queue" );

	// Create timers.
	xUD_Task_1_Release_Timer = xTimerCreate( "UD_Task_1_Release_Timer",
									 	     pdMS_TO_TICKS(UD_TASK_1_PERIOD),
										     pdTRUE,
										     (void *) 0,
										     Enqueue_UD_Task_1_Release_Timer_Flag);

	xUD_Task_2_Release_Timer = xTimerCreate( "UD_Task_2_Release_Timer",
									 	     pdMS_TO_TICKS(UD_TASK_2_PERIOD),
										     pdTRUE,
										     (void *) 0,
										     Enqueue_UD_Task_2_Release_Timer_Flag);

	xUD_Task_3_Release_Timer = xTimerCreate( "UD_Task_3_Release_Timer",
									 	     pdMS_TO_TICKS(UD_TASK_3_PERIOD),
										     pdTRUE,
										     (void *) 0,
										     Enqueue_UD_Task_3_Release_Timer_Flag);

	xMonitor_Request_Timer = xTimerCreate(   "Monitor_Request_timer",
									 	     pdMS_TO_TICKS(MONITOR_REQUEST_PERIOD),
										     pdTRUE,
										     (void *) 0,
										     Enqueue_Monitor_Request);

	// Start the tasks and timer running.
	vTaskStartScheduler();

	return 0;
}

/*-----------------------------------------------------------*/
// DD Task Generator Task.

static void DD_Task_Generator_Task( void *pvParameters )
{
	// Prepare task handles.
	TaskHandle_t UD_Task_1_Handle = NULL;
	TaskHandle_t UD_Task_2_Handle = NULL;
	TaskHandle_t UD_Task_3_Handle = NULL;

	// Prepare data items.
	uint16_t ud_task = -1;
	uint16_t ud_task_id = 0;
	TickType_t release_time;
	TickType_t absolute_deadline;

	// Start task timers.
	xTimerStart(xUD_Task_1_Release_Timer, UD_TASK_1_PERIOD);
	xTimerStart(xUD_Task_2_Release_Timer, UD_TASK_2_PERIOD);
	xTimerStart(xUD_Task_3_Release_Timer, UD_TASK_3_PERIOD);

	// Perform initial release of all tasks.
	Enqueue_UD_Task_1_Release_Timer_Flag(xUD_Task_1_Release_Timer);
	Enqueue_UD_Task_2_Release_Timer_Flag(xUD_Task_2_Release_Timer);
	Enqueue_UD_Task_3_Release_Timer_Flag(xUD_Task_3_Release_Timer);

	while(1)
	{
		// Wait for flag from any task timer.
		xQueueReceive(xTask_Generator_Queue, &ud_task, portMAX_DELAY);

		switch(ud_task)
		{
				case UD_TASK_1:
					// Inform DDS of task 1 release.
					release_time = xTaskGetTickCount();
					absolute_deadline = release_time + pdMS_TO_TICKS(UD_TASK_1_PERIOD);
					Release_DD_Task(UD_TASK_1, UD_Task_1_Handle, PERIODIC, ud_task_id, release_time, absolute_deadline);
					break;
				case UD_TASK_2:
					// Inform DDS of task 2 release.
					release_time = xTaskGetTickCount();
					absolute_deadline = release_time + pdMS_TO_TICKS(UD_TASK_2_PERIOD);
					Release_DD_Task(UD_TASK_2, UD_Task_2_Handle, PERIODIC, ud_task_id, release_time, absolute_deadline);
					break;
				case UD_TASK_3:
					// Inform DDS of task 3 release.
					release_time = xTaskGetTickCount();
					absolute_deadline = release_time + pdMS_TO_TICKS(UD_TASK_3_PERIOD);
					Release_DD_Task(UD_TASK_3, UD_Task_3_Handle, PERIODIC, ud_task_id, release_time, absolute_deadline);
					break;
		}

		// Increment id.
		ud_task_id++;
	}
}

// Task generator timer callback functions.
static void Enqueue_UD_Task_1_Release_Timer_Flag(xTimerHandle pxTimer)
{
	// Task 1 period timer restarts by autoreload.

	uint16_t flag = UD_TASK_1;
	xQueueSend(xTask_Generator_Queue, &flag, UD_TASK_1_PERIOD);
}

static void Enqueue_UD_Task_2_Release_Timer_Flag(xTimerHandle pxTimer)
{
	// Task 2 period timer restarts by autoreload.

	uint16_t flag = UD_TASK_2;
	xQueueSend(xTask_Generator_Queue, &flag, UD_TASK_2_PERIOD);
}

static void Enqueue_UD_Task_3_Release_Timer_Flag(xTimerHandle pxTimer)
{
	// Task 2 period timer restarts by autoreload.

	uint16_t flag = UD_TASK_3;
	xQueueSend(xTask_Generator_Queue, &flag, UD_TASK_3_PERIOD);
}

/*-----------------------------------------------------------*/
// Monitor Task.

static void Monitor_Task(void *pvParameters)
{
	struct Monitor_Message monitor_message;
	TickType_t current_time;

	// Start monitor request timer.
	xTimerStart(xMonitor_Request_Timer, MONITOR_REQUEST_PERIOD);

	while(1)
	{
		// Wait for message on monitor queue.
		xQueueReceive(xMonitor_Queue, &monitor_message, portMAX_DELAY);

		switch(monitor_message.message_type)
		{
			case TASK_RELEASE: // We have a release time to print.
				if (print_release_times)
				{
					printf("%d %s %d\n", (int) monitor_message.time, "R", monitor_message.ud_task);

				}
				break;
			case TASK_COMPLETE: // We have a completion time to print.
				if (print_completion_times)
				{
					printf("%d %s %d\n", (int) monitor_message.time, "C", monitor_message.ud_task);
				}
				break;
			case TASK_OVERDUE: // We have an overdue time to print.
				if (print_overdue_times)
				{
					printf("%d %s %d\n", (int) monitor_message.time, "O", monitor_message.ud_task);
				}
				break;
			case MONITOR_REQUEST_TASK_COUNTS: // We have a received the requested list counts.
				if (print_dd_task_list_sizes)
				{
					current_time = xTaskGetTickCount();
					printf("%d %s %d %d %d\n", (int) current_time, "counts:", monitor_message.active_count, monitor_message.completed_count, monitor_message.overdue_count);
				}
				break;
			case MONITOR_REQUEST_TIMER_EXPIRED: // We need to request list counts.
				Request_DD_Task_Counts();
				break;
		}
	}
}

// Monitor task interface functions.
// Direct monitor to print a release, completion, or overdue time.
static void Print_Event_Time(uint16_t event_type, uint16_t ud_task, TickType_t time)
{
	struct Monitor_Message monitor_message;
	monitor_message.message_type = event_type;
	monitor_message.ud_task = ud_task;
	monitor_message.time = time;

	xQueueSend(xMonitor_Queue, &monitor_message, pdMS_TO_TICKS(QUEUE_WAIT_TIME));
}

// Direct monitor to print list sizes.
static void Print_DD_Task_List_Sizes(uint16_t active_count, uint16_t completed_count, uint16_t overdue_count)
{
	struct Monitor_Message monitor_message;
	monitor_message.message_type = MONITOR_REQUEST_TASK_COUNTS;
	monitor_message.active_count = active_count;
	monitor_message.completed_count = completed_count;
	monitor_message.overdue_count = overdue_count;

	xQueueSend(xMonitor_Queue, &monitor_message, pdMS_TO_TICKS(QUEUE_WAIT_TIME));
}

// Monitor request timer callback function.
static void Enqueue_Monitor_Request(xTimerHandle pxTimer)
{
	// Monitor request timer restarts by autoreload.

	uint16_t flag = MONITOR_REQUEST_TIMER_EXPIRED;
	xQueueSend(xMonitor_Queue, &flag, pdMS_TO_TICKS(QUEUE_WAIT_TIME));
}

/*-----------------------------------------------------------*/
// DDS Task.

static void DDS_Task( void *pvParameters )
{
	// Prepare task lists.
	struct DD_Task *active_list_head = NULL;
	struct DD_Task *completed_list_head = NULL;
	struct DD_Task *overdue_list_head = NULL;
	uint16_t active_count = 0;
	uint16_t completed_count = 0;
	uint16_t overdue_count = 0;

	struct DDS_Message dds_message;

	while(1)
	{
		// Wait for message on dds queue.
		xQueueReceive(xDDS_Queue, &dds_message, portMAX_DELAY);

		if (dds_message.message_type == TASK_RELEASE)
		{
			// Prepare struct defining task.
			struct DD_Task *dd_task = (struct DD_Task*) pvPortMalloc(sizeof(struct DD_Task));
			dd_task->ud_task = dds_message.ud_task;
			dd_task->t_handle = dds_message.t_handle;
			dd_task->task_type = dds_message.task_type;
			dd_task->task_id = dds_message.task_id;
			dd_task->release_time = dds_message.release_time;
			dd_task->absolute_deadline = dds_message.absolute_deadline;
			dd_task->started = false;
			dd_task->next = NULL;

			// Add task to active list.
			if (dd_task->task_type == PERIODIC) Enlist_Task_By_Deadline(&active_list_head, dd_task); // If periodic, sort by deadline.
			else Enlist_Task(&active_list_head, dd_task); // If aperiodic, send to back of list.
			active_count++;

			// Start next task.
			Schedule_Next_Task(active_list_head);

			if (print_release_times) Print_Event_Time(TASK_RELEASE, dds_message.ud_task, dds_message.release_time);
		}
		else if (dds_message.message_type == TASK_COMPLETE)
		{
			// Set completion_time of head.
			active_list_head->completion_time = dds_message.completion_time;

			// Delete the F-task instance corresponding to head.
			vTaskDelete(active_list_head->t_handle);

			// Check if overdue, move to appropriate list, and delete from active list.
			if (active_list_head->completion_time <= active_list_head->absolute_deadline)
			{
				if (print_completion_times) Print_Event_Time(TASK_COMPLETE, active_list_head->ud_task, active_list_head->completion_time);
				Enlist_Task(&completed_list_head, Delist_Task(&active_list_head));
				completed_count++;
			}
			else
			{
				if (print_overdue_times) Print_Event_Time(TASK_OVERDUE, active_list_head->ud_task, active_list_head->completion_time);
				Enlist_Task(&overdue_list_head, Delist_Task(&active_list_head));
				overdue_count++;
			}
			active_count--;

			// Schedule next.
			Schedule_Next_Task(active_list_head);
		}
		else if (dds_message.message_type == MONITOR_REQUEST_TASK_COUNTS)
		{
			Print_DD_Task_List_Sizes(active_count, completed_count, overdue_count);
		}
	}
}

// DDS helper functions.
static void Enlist_Task_By_Deadline(struct DD_Task** list_head, struct DD_Task* dd_task)
{
	// Check if list empty.
	if (*list_head == NULL)
	{
		*list_head = dd_task;
	}
	else
	{
		// Check if must add to front.
		if ((*list_head)->task_type == APERIODIC || (*list_head)->absolute_deadline > dd_task->absolute_deadline)
		{
			dd_task->next = (*list_head);
			*list_head = dd_task;
		}
		// Else sort into list by ascending absolute_deadline.
		else
		{
			struct DD_Task *current = *list_head;
			while (current->next != NULL && current->next->task_type != APERIODIC && current->next->absolute_deadline <= dd_task->absolute_deadline)
			{
				current = current->next;
			}
			dd_task->next = current->next;
			current->next = dd_task;
		}
	}
}

static void Enlist_Task(struct DD_Task** list_head, struct DD_Task* dd_task)
{
	if (*list_head == NULL)
	{
		*list_head = dd_task;
	}
	else
	{
		struct DD_Task *current = *list_head;
		while (current->next != NULL) current = current->next;
		current->next = dd_task;
	}
}

static struct DD_Task* Delist_Task(struct DD_Task** list_head)
{
	if (*list_head == NULL) return NULL;

	struct DD_Task* temp = *list_head;
	*list_head = (*list_head)->next;
	temp->next = NULL;
	return temp;
}

static void Schedule_Next_Task(struct DD_Task* list_head)
{
	if (list_head == NULL) return;
	if (!list_head->started)
	{
		switch(list_head->ud_task)
		{
			case UD_TASK_1:
				xTaskCreate(UD_Task_1, "UD_Task_1", configMINIMAL_STACK_SIZE, NULL, 1, &list_head->t_handle);
				break;
			case UD_TASK_2:
				xTaskCreate(UD_Task_2, "UD_Task_2", configMINIMAL_STACK_SIZE, NULL, 1, &list_head->t_handle);
				break;
			case UD_TASK_3:
				xTaskCreate(UD_Task_3, "UD_Task_3", configMINIMAL_STACK_SIZE, NULL, 1, &list_head->t_handle);
				break;
		}
		list_head->started = true;
	}

	// Raise priority of head task.
	vTaskPrioritySet(list_head->t_handle, 2);

	// Lower priority of head task in the list, in case it had been running.
	if (list_head->next != NULL && list_head->next->started)
	{
		vTaskPrioritySet(list_head->next->t_handle, 1);
	}
}

// DDS interface functions.

static void Release_DD_Task(uint16_t ud_task, TaskHandle_t t_handle, uint16_t task_type, uint32_t task_id, TickType_t release_time, TickType_t absolute_deadline)
{
	// Create task release message struct.
	struct DDS_Message task_release_message;
	task_release_message.message_type = TASK_RELEASE;
	task_release_message.ud_task = ud_task;
	task_release_message.t_handle = t_handle;
	task_release_message.task_type = task_type;
	task_release_message.task_id = task_id;
	task_release_message.release_time = release_time;
	task_release_message.absolute_deadline = absolute_deadline;

	// Add message to DDS queue.
	xQueueSend(xDDS_Queue, &task_release_message, pdMS_TO_TICKS(QUEUE_WAIT_TIME));
}


static void Complete_DD_Task(TickType_t completion_time)
{
	// Prepare task complete message struct.
	struct DDS_Message task_complete_message;
	task_complete_message.message_type = TASK_COMPLETE;
	task_complete_message.completion_time = completion_time;

	// Add message to DDS queue.
	xQueueSend(xDDS_Queue, &task_complete_message, pdMS_TO_TICKS(QUEUE_WAIT_TIME));
}

static void Request_DD_Task_Counts()
{
	// Prepare task count message struct.
	struct DDS_Message task_counts_request_message;
	task_counts_request_message.message_type = MONITOR_REQUEST_TASK_COUNTS;
	// Add message to DDS queue.
	xQueueSend(xDDS_Queue, &task_counts_request_message, pdMS_TO_TICKS(QUEUE_WAIT_TIME));
}

/*-----------------------------------------------------------*/
// User-defined f-tasks.

static void UD_Task_1( void *pvParameters )
{
	TickType_t execution_time_ticks = pdMS_TO_TICKS(UD_TASK_1_EXECUTION_TIME);
	TickType_t ticks_elapsed = 0;
	TickType_t last_time_ticks = xTaskGetTickCount();
	TickType_t current_time_ticks;

	while(1)
	{
		// Execute "user-defined code" for duration specified by UD_TASK_1_EXECUTION_TIME.
		while(1)
		{
			current_time_ticks = xTaskGetTickCount();
			if (current_time_ticks != last_time_ticks)
			{
				ticks_elapsed++;
				last_time_ticks = current_time_ticks;
			}
			if (ticks_elapsed >= execution_time_ticks) break;
		}
		Complete_DD_Task(current_time_ticks);
	}
}

static void UD_Task_2( void *pvParameters )
{
	TickType_t execution_time_ticks = pdMS_TO_TICKS(UD_TASK_2_EXECUTION_TIME);
	TickType_t ticks_elapsed = 0;
	TickType_t last_time_ticks = xTaskGetTickCount();
	TickType_t current_time_ticks;

	while(1)
	{
		// Execute "user-defined code" for duration specified by UD_TASK_2_EXECUTION_TIME.
		while(1)
		{
			current_time_ticks = xTaskGetTickCount();
			if (current_time_ticks != last_time_ticks)
			{
				ticks_elapsed++;
				last_time_ticks = current_time_ticks;
			}
			if (ticks_elapsed >= execution_time_ticks) break;
		}
		Complete_DD_Task(current_time_ticks);
	}
}

static void UD_Task_3( void *pvParameters )
{
	TickType_t execution_time_ticks = pdMS_TO_TICKS(UD_TASK_3_EXECUTION_TIME);
	TickType_t ticks_elapsed = 0;
	TickType_t last_time_ticks = xTaskGetTickCount();
	TickType_t current_time_ticks;

	while(1)
	{
		// Execute "user-defined code" for duration specified by UD_TASK_3_EXECUTION_TIME.
		while(1)
		{
			current_time_ticks = xTaskGetTickCount();
			if (current_time_ticks != last_time_ticks)
			{
				ticks_elapsed++;
				last_time_ticks = current_time_ticks;
			}
			if (ticks_elapsed >= execution_time_ticks) break;
		}
		Complete_DD_Task(current_time_ticks);
	}
}


/*-----------------------------------------------------------*/
// Built-in functions.

void vApplicationMallocFailedHook( void )
{
	/* The malloc failed hook is enabled by setting
	configUSE_MALLOC_FAILED_HOOK to 1 in FreeRTOSConfig.h.

	Called if a call to pvPortMalloc() fails because there is insufficient
	free memory available in the FreeRTOS heap.  pvPortMalloc() is called
	internally by FreeRTOS API functions that create tasks, queues, software
	timers, and semaphores.  The size of the FreeRTOS heap is set by the
	configTOTAL_HEAP_SIZE configuration constant in FreeRTOSConfig.h. */
	//for( ;; );
	printf("%s\n", "malloc failed");
}
/*-----------------------------------------------------------*/

void vApplicationStackOverflowHook( xTaskHandle pxTask, signed char *pcTaskName )
{
	( void ) pcTaskName;
	( void ) pxTask;

	/* Run time stack overflow checking is performed if
	configconfigCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2.  This hook
	function is called if a stack overflow is detected.  pxCurrentTCB can be
	inspected in the debugger if the task name passed into this function is
	corrupt. */
	for( ;; );
}
/*-----------------------------------------------------------*/

void vApplicationIdleHook( void )
{
volatile size_t xFreeStackSpace;

	/* The idle task hook is enabled by setting configUSE_IDLE_HOOK to 1 in
	FreeRTOSConfig.h.

	This function is called on each cycle of the idle task.  In this case it
	does nothing useful, other than report the amount of FreeRTOS heap that
	remains unallocated. */
	xFreeStackSpace = xPortGetFreeHeapSize();

	if( xFreeStackSpace > 100 )
	{
		/* By now, the kernel has allocated everything it is going to, so
		if there is a lot of heap remaining unallocated then
		the value of configTOTAL_HEAP_SIZE in FreeRTOSConfig.h can be
		reduced accordingly. */
	}
}
