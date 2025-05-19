#include <stdint.h>
#include <errno.h>
#include <ucontext.h>
#include <ucontext.h>
#include <valgrind/memcheck.h>
#include <assert.h>
#include <sys/types.h>
#include "queue.h"
#include <stdlib.h>
#include "kfc.h"

void scheduler(void);

static int inited = 0;
int numThreadsCreated = KFC_TID_MAIN;
int _currentTID = 0; // I like dart's naming convention for global variables
ucontext_t contexts[KFC_MAX_THREADS];
queue_t theQueue ;
ucontext_t scheduler_c;
void *threadRets[KFC_MAX_THREADS];
int threadReted[KFC_MAX_THREADS];
queue_t *threadWaitQs[KFC_MAX_THREADS];

/**
 * Initializes the kfc library.  Programs are required to call this function
 * before they may use anything else in the library's public interface.
 *
 * @param kthreads    Number of kernel threads (pthreads) to allocate
 * @param quantum_us  Preemption timeslice in microseconds, or 0 for cooperative
 *                    scheduling
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_init(int kthreads, int quantum_us)
{
	assert(!inited);
	for (int i = 0; i < KFC_MAX_THREADS; i++) {
		threadRets[i] = NULL;
		threadReted[i] = 0;
		threadWaitQs[i] = NULL;
	}
	queue_init(&theQueue );
	scheduler();
	inited = 1;
	getcontext(contexts);
	return 0;
}

void
scheduler(void)
{
	getcontext(&scheduler_c);
	DPRINTF("scheduler\n");
	if (!inited) return;
	if(queue_size(&theQueue ) == 0) {
		DPRINTF("QUEUE EMPTYYYY");
		_currentTID = 0;
		return;
	}
	tid_t next_tid = (tid_t)(intptr_t)queue_dequeue(&theQueue ) - 1;
	DPRINTF("--------\ndequeued and now %d\n", next_tid);
	_currentTID = next_tid;
	setcontext(&contexts[next_tid]);
}

void
trampoline(void *(*start_func)(void *), void *arg)
{
	DPRINTF("trampoline\n");
	kfc_exit(start_func(arg));
}
/**
 * Cleans up any resources which were allocated by kfc_init.  You may assume
 * that this function is called only from the main thread, that any other
 * threads have terminated and been joined, and that threading will not be
 * needed again.  (In other words, just clean up and don't worry about the
 * consequences.)
 *
 * I won't be testing this function specifically, but it is provided as a
 * convenience to you if you are using Valgrind to check your code, which I
 * always encourage.
 */
void
kfc_teardown(void)
{
	assert(inited);
	DPRINTF("teardown\n");
	inited = 0;
	queue_destroy(&theQueue);
	for (int i = 0; i < numThreadsCreated; i++)
		if (threadWaitQs[i] != NULL) {
			queue_destroy(threadWaitQs[i]);
			free(threadWaitQs[i]);
		}
}

/**
 * Creates a new user thread which executes the provided function concurrently.
 * It is left up to the implementation to decide whether the calling thread
 * continues to execute or the new thread takes over immediately.
 *
 * @param ptid[out]   Pointer to a tid_t variable in which to store the new
 *                    thread's ID
 * @param start_func  Thread main function
 * @param arg         Argument to be passed to the thread main function
 * @param stack_base  Location of the thread's stack if already allocated, or
 *                    NULL if requesting that the library allocate it
 *                    dynamically
 * @param stack_size  Size (in bytes) of the thread's stack, or 0 to use the
 *                    default thread stack size KFC_DEF_STACK_SIZE
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_create(tid_t *ptid, void *(*start_func)(void *), void *arg,
		caddr_t stack_base, size_t stack_size)
{
	assert(inited);
	DPRINTF("create\n");
	int preCreateID = _currentTID;
	*ptid = ++numThreadsCreated;
	_currentTID = *ptid;
	DPRINTF("Creating%d\n", *ptid);

	DPRINTF("Enqueueing thread %d\n", *ptid);
	int rv = queue_enqueue(&theQueue , (void *)(uintptr_t)(1 + *ptid));
	if (rv != 0) {
		perror("Failed to enqueue thread");
		DPRINTF("Failed to enqueue thread^%d", errno);
		return -1;
	}

	if (stack_size == 0) stack_size = KFC_DEF_STACK_SIZE;
	if (stack_base == NULL) {
		stack_base = malloc(stack_size);
		VALGRIND_STACK_REGISTER(stack_base, stack_base + stack_size);
	}
	contexts[*ptid].uc_stack.ss_sp = stack_base;
	contexts[*ptid].uc_stack.ss_size = stack_size;
	contexts[*ptid].uc_link = &scheduler_c;
	getcontext(&contexts[*ptid]);
	makecontext(&contexts[*ptid], trampoline, 2, (void (*)()) start_func , (long) arg);
	
	_currentTID = preCreateID;
	return 0;
}

/**
 * Exits the calling thread.  This should be the same thing that happens when
 * the thread's start_func returns.
 *
 * @param ret  Return value from the thread
 */
void
kfc_exit(void *ret)
{
	assert(inited);
	DPRINTF("exit\n");
	threadRets[_currentTID] = ret;
	threadReted[_currentTID] = 1;
	if (threadWaitQs[_currentTID] != NULL)
		while (queue_size(threadWaitQs[_currentTID]) > 0) {
			int waiter = queue_dequeue(threadWaitQs[_currentTID]);
			queue_enqueue(&theQueue , waiter);
		}
	// DPRINTF("_currentTID exiting: %d", _currentTID);
	setcontext(&scheduler_c);
}

/**
 * Waits for the thread specified by tid to terminate, retrieving that threads
 * return value.  Returns immediately if the target thread has already
 * terminated, otherwise blocks.  Attempting to join a thread which already has
 * another thread waiting to join it, or attempting to join a thread which has
 * already been joined, results in undefined behavior.
 *
 * @param pret[out]  Pointer to a void * in which the thread's return value from
 *                   kfc_exit should be stored, or NULL if the caller does not
 *                   care.
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_join(tid_t tid, void **pret)
{
	assert(inited);
	DPRINTF("join\n");

	if (!threadReted[tid]) {//wait
		if (threadWaitQs[tid] == NULL) {
			threadWaitQs[tid] = (queue_t *)malloc(sizeof(queue_t));
			queue_init(threadWaitQs[tid]);
		}
		queue_enqueue(threadWaitQs[tid], (void *)(uintptr_t)(1 + _currentTID));
		swapcontext(&contexts[_currentTID], &scheduler_c);
	}
	assert(threadReted[tid]);

	free(contexts[tid].uc_stack.ss_sp);
	if (pret != NULL)
		*pret = threadRets[tid];

	return 0;
}

/**
 * Returns a small integer which identifies the calling thread.
 *
 * @return Thread ID of the currently executing thread
 */
tid_t
kfc_self(void)
{
	DPRINTF("self\n");
	assert(inited);
	// DPRINTF("kfc_self ret %d. numThreads%d\n", _currentTID, numThreadsCreated);
	return _currentTID;
}

/**
 * Causes the calling thread to yield the processor voluntarily.  This may
 * result in another thread being scheduled, but it does not preclude the
 * possibility of the same thread continuing if re-chosen by the scheduling
 * algorithm.
 */
void
kfc_yield(void)
{
	DPRINTF("yield\n");
	int rv = queue_enqueue(&theQueue , (void *)(uintptr_t)(1 + _currentTID));
	if (rv != 0) {
		perror("Failed to enqueue thread");
		return;
	}
	swapcontext(&contexts[_currentTID], &scheduler_c);
	assert(inited);
}

/**
 * Initializes a user-level counting semaphore with a specific value.
 *
 * @param sem    Pointer to the semaphore to be initialized
 * @param value  Initial value for the semaphore's counter
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_sem_init(kfc_sem_t *sem, int value)
{
	assert(inited);
	queue_init(&sem->waitQ);
	sem->count= value;
	return 0;
}

/**
 * Increments the value of the semaphore.  This operation is also known as
 * up, signal, release, and V (Dutch verhoog, "increase").
 *
 * @param sem  Pointer to the semaphore which the thread is releasing
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_sem_post(kfc_sem_t *sem)
{
	assert(inited);
	if (sem->count != 0) {
		assert(sem->waitQ.size == 0);
		sem->count++;
		return 0;
	}

	if (sem->waitQ.size) {
		int waiter = queue_dequeue(&sem->waitQ);
		queue_enqueue(&theQueue, waiter);
	} else {
		sem->count++;
	}
	return 0;
}

/**
 * Attempts to decrement the value of the semaphore.  This operation is also
 * known as down, acquire, and P (Dutch probeer, "try").  This operation should
 * block when the counter is not above 0.
 *
 * @param sem  Pointer to the semaphore which the thread wishes to acquire
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_sem_wait(kfc_sem_t *sem)
{
	DPRINTF("sem\n");
	assert(inited);
	if (sem->count) {
		sem->count--;
		assert(sem->count >= 0);
		return 0;
	}
	queue_enqueue(&sem->waitQ, (void *)(uintptr_t)(_currentTID + 1));
	swapcontext(&contexts[_currentTID], &scheduler_c);

	return 0;
}

/**
 * Frees any resources associated with a semaphore.  Destroying a semaphore on
 * which threads are waiting results in undefined behavior.
 *
 * @param sem  Pointer to the semaphore to be destroyed
 */
void
kfc_sem_destroy(kfc_sem_t *sem)
{
	assert(inited);
	queue_destroy(&sem->waitQ);
}
