/*
 *    Name: ZVector
 * Purpose: Library to use Dynamic Arrays (Vectors) in C Language
 *  Author: Paolo Fabio Zaino
 *  Domain: General
 * License: Copyright by Paolo Fabio Zaino, all rights reserved
 *          Distributed under MIT license
 *
 * Credits: This Library was inspired by the work of quite few,
 *          apologies if I forgot to  mention them all!
 *
 *          Gnome Team (GArray demo)
 *          Dimitros Michail (Dynamic Array in C presentation)
 *
 */

/*
 * Few code standard notes:
 *
 * p_ <- indicate a PRIVATE method or variable. Not reachable outside this module.
 *
 */

// Include standard C libs headers
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Include vector.h header
#include "zvector.h"

#if (OS_TYPE == 1)
#	if (!defined(macOS))
/*  Improve PThreads on Linux.
 *  macOS seems  to be  handling pthreads
 *  in a   slightly  different   way than
 *  Linux, so, avoid using the same trick
 *  on macOS.
 */
#	ifndef _POSIX_C_SOURCE
#		define _POSIX_C_SOURCE 200112L
#	endif // _POSIX_C_SOURCE
#	define __USE_UNIX98
#	endif // macOS
#endif // OS_TYPE

// Include non-ANSI Libraries
// only if the user has requested
// special extensions:
#if (OS_TYPE == 1)
#	if ( !defined(macOS) )
#		include <malloc.h>
#	endif
#	if (CPU_TYPE == x86_64)
//#		include <xmmintrin.h>
#	endif
#endif // OS_TYPE
#if (ZVECT_THREAD_SAFE == 1)
#	if MUTEX_TYPE == 1
#		include <pthread.h>
#	elif MUTEX_TYPE == 2
#		include <windows.h>
#		include <psapi.h>
#	endif // MUTEX_TYPE
#endif // ZVECT_THREAD_SAFE

// Local Defines/Macros:

// Declare Vector status flags:
enum {
	ZVS_NONE = 0,              // Set or Reset vector's status register.
	ZVS_CUST_WIPE_ON = 1 << 0  // Sets the bit to indicate a custom secure wipe
				   // function has been set.
};

#ifndef ZVECT_MEMX_METHOD
#	define ZVECT_MEMX_METHOD 1
#endif

#if defined(Arch32)
#	define ADDR_TYPE1 uint32_t
#	define ADDR_TYPE2 uint32_t
#	define ADDR_TYPE3 uint16_t
#else
#	define ADDR_TYPE1 uint64_t
#	define ADDR_TYPE2 uint64_t
#	define ADDR_TYPE3 uint32_t
#endif // Arch32

/*---------------------------------------------------------------------------*/
// Useful macros
#define min(x, y) (((x) < (y)) ? (x) : (y))
#define max(x, y) (((x) > (y)) ? (x) : (y))
#define UNUSED(x) (void)x

/*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
// Define the vector data structure:

// This is ZVector core data structure, it is the structure of a ZVector vector :)
// Please note: fields order is based on "most used fields" to help a bit with cache
struct p_vector {
	zvect_index cap_left;		// - Max capacity allocated on the left.
	zvect_index cap_right;		// - Max capacity allocated on the right.
	zvect_index begin;		// - First vector's Element Pointer
	zvect_index prev_end;		// - Used when clearing a vector.
	zvect_index end;		// - Current Array size. size - 1 gives
					//   us the pointer to the last element
					//   in the vector.
	size_t data_size;		// - User DataType size.
					//   This should be 2 bytes size on a
					//   16-bit system, 4 bytes on a 32 bit,
					//   8 bytes on a 64 bit. But check your
					//   compiler for the actual size, it's
					//   implementation dependent.
	uint32_t flags;			// - This flag set is used to store all
					//   Vector's properties.
					//   It contains bits that set Secure
					//   Wipe, Auto Shrink, Pass Items By
					//   Ref etc.
#if (ZVECT_THREAD_SAFE == 1)
#	if MUTEX_TYPE == 0
	void *lock;			// - Vector's mutex for thread safe
					//   micro-transactions or user locks.
					//   This should be 2 bytes size on a
					//   16 bit machine, 4 bytes on a 32 bit
					//   4 bytes on a 64 bit.
#	elif MUTEX_TYPE == 1
	pthread_mutex_t lock;		// - Vector's mutex for thread safe
					//   micro-transactions or user locks.
					//   This should be 24 bytes on a 32bit
					//   machine and 40 bytes on a 64bit.
#	elif MUTEX_TYPE == 2
	CRITICAL_SECTION lock;		// - Vector's mutex for thread safe
					//   micro-transactions or user locks.
					//   Check your WINNT.H to calculate the
					//   size of this one.
#	endif  // MUTEX_TYPE
#endif  // ZVECT_THREAD_SAFE
	void **data ZVECT_DATAALIGN;	// - Vector's storage.
	zvect_index init_capacity;	// - Initial Capacity (this is set at
					//   creation time).
					//   For the size of zvect_index check
					//   zvector_config.h.
	uint32_t status;		// - Internal vector Status Flags
	void (*SfWpFunc)(const void *item, size_t size);
					// - Pointer to a CUSTOM Safe Wipe
					//   function (optional) needed only
					//   for Secure Wiping special
					//   structures.
#ifdef ZVECT_DMF_EXTENSIONS
	zvect_index balance;		// - Used by the Adaptive Binary Search
					//   to improve performance.
	zvect_index bottom;	 	// - Used to optimise Adaptive Binary
					//   Search.
#endif  // ZVECT_DMF_EXTENSIONS
#if (ZVECT_THREAD_SAFE == 1)		// - Leave this always at the end.
	volatile int32_t lock_type;	// - This field contains the lock type
					//   used for this Vector. Because it's
					//   a volatile field, it won't be cached
					//   so leave it at the end of this struct
#endif  // ZVECT_THREAD_SAFE
} ZVECT_DATAALIGN;

// Initialisation state:
static uint32_t p_init_state = 0;

/*---------------------------------------------------------------------------*/

/*****************************************************************************
 **                       ZVector Support Functions                         **
 *****************************************************************************/

/*---------------------------------------------------------------------------*/
// Errors and messages handling:

#if (ZVECT_COMPTYPE == 1) || (ZVECT_COMPTYPE == 3)
__attribute__((noreturn))
#endif
static void p_throw_error(const zvect_retval error_code, const char *error_message) {
	int32_t locally_allocated = 0;
	char *message;
	unsigned long msg_len = 0;
	if ( error_message == NULL )
	{
		msg_len = sizeof(char)*255;
		message=(char *)malloc(msg_len);
		locally_allocated=-1;
	} else {
		msg_len = strlen(error_message);
		message=(char *)malloc(sizeof(char) * (msg_len + 1));
	}
	if ( error_message == NULL ) {
		switch (error_code)
		{
			case ZVERR_VECTUNDEF:
				strncpy(message, "Undefined or uninitialized vector.\n\0", msg_len);
				break;
			case ZVERR_IDXOUTOFBOUND:
				strncpy(message, "Index out of bound.\n\0", msg_len);
				break;
			case ZVERR_OUTOFMEM:
				strncpy(message, "Not enough memory to allocate space for the vector.\n\0", msg_len);
				break;
			case ZVERR_VECTCORRUPTED:
				strncpy(message, "Vector corrupted.\n\0", msg_len);
				break;
			case ZVERR_RACECOND:
				strncpy(message, "Race condition detected, cannot continue.\n\0", msg_len);
				break;
			case ZVERR_VECTTOOSMALL:
				strncpy(message, "Destination vector is smaller than source.\n\0", msg_len);
				break;
			case ZVERR_VECTDATASIZE:
				strncpy(message, "This operation requires two (or more vectors) with the same data size.\n\0", msg_len);
				break;
			case ZVERR_VECTEMPTY:
				strncpy(message, "Vector is empty.\n\0", msg_len);
				break;
			default:
				strncpy(message, "Unknown error.\n\0", msg_len);
				break;
		}
	} else
		strncpy(message, error_message, msg_len);

#if OS_TYPE == 1
	fprintf(stderr, "Error: %i, %s", error_code, message);
	if (locally_allocated)
		free(message);
	exit(error_code);
#else
	printf("Error: i%, %s\n", error_code, error_message);
	if (locally_allocated)
		free(error_message);
	exit(error_code);
#endif // OS_TYPE
}

/*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
// Memory management:

#if (ZVECT_MEMX_METHOD == 0)
static inline
#endif // ZVECT_MEMX_METHOD
void *p_vect_memcpy(void *__restrict dst, const void *__restrict src, size_t size) {
#if (ZVECT_MEMX_METHOD == 0)
	// Using regular memcpy
	// If you are using ZVector on Linux/macOS/BSD/Windows
	// you should stick to this one!
	return memcpy(dst, src, size);
#elif (ZVECT_MEMX_METHOD == 1)
	// Using improved memcpy (where improved means for
	// embedded systems only!):
	if (size > 0) {
		if (((uintptr_t)dst % sizeof(ADDR_TYPE1) == 0) &&
		    ((uintptr_t)src % sizeof(ADDR_TYPE1) == 0) &&
		    (size % sizeof(ADDR_TYPE1) == 0)) {
			ADDR_TYPE1 *pExDst = (ADDR_TYPE1 *)dst;
			ADDR_TYPE1 const *pExSrc = (ADDR_TYPE1 const *)src;
			size_t end = size / sizeof(ADDR_TYPE1);
			for (register size_t i = 0; i < end; i++) {
				// The following should be compiled as: (-O2 on x86_64)
				//         mov     rdi, QWORD PTR [rsi+rcx]
				//         mov     QWORD PTR [rax+rcx], rdi
				*pExDst++ = *pExSrc++;
			}
		} else {
			return memcpy(dst, src, size);
		}
	}
	return dst;
#endif // ZVECT_MEMX_METHOD
}

static inline void *vect_memmove(void *__restrict dst,
                                 const void *__restrict src, size_t size) {
	return memmove(dst, src, size);
}

/*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
// Thread Safe functions:

#if (ZVECT_THREAD_SAFE == 1)
static volatile bool lock_enabled = true;
#	if MUTEX_TYPE == 0
#		define ZVECT_THREAD_SAFE 0
#	elif MUTEX_TYPE == 1
static inline void mutex_lock(pthread_mutex_t *lock) {
	pthread_mutex_lock(lock);
}

static inline void mutex_unlock(pthread_mutex_t *lock) {
	pthread_mutex_unlock(lock);
}

static inline void mutex_alloc(pthread_mutex_t *lock) {
#	if (!defined(macOS))
	pthread_mutexattr_t Attr;
	pthread_mutexattr_init(&Attr);
	pthread_mutexattr_settype(&Attr, PTHREAD_MUTEX_RECURSIVE_NP);
	pthread_mutex_init(lock, &Attr);
#	else
	pthread_mutex_init(lock, NULL);
#	endif // macOS
}

static inline void mutex_destroy(pthread_mutex_t *lock) {
	pthread_mutex_unlock(lock);
	pthread_mutex_destroy(lock);
}

#	elif MUTEX_TYPE == 2

static inline void mutex_lock(CRITICAL_SECTION *lock) {
	EnterCriticalSection(lock);
}

static inline void mutex_unlock(CRITICAL_SECTION *lock) {
	LeaveCriticalSection(lock);
}

static inline void mutex_alloc(CRITICAL_SECTION *lock) {
	// InitializeCriticalSection(lock);
	InitializeCriticalSectionAndSpinCount(lock, 32);
}

static inline void mutex_destroy(CRITICAL_SECTION *lock) {
	DeleteCriticalSection(lock);
}
#	endif // MUTEX_TYPE
#endif // ZVECT_THREAD_SAFE

#if (ZVECT_THREAD_SAFE == 1)
// The following two functions are generic locking functions

/*
 * ZVector uses the concept of Priorities for locking.
 * A user lock has the higher priority while ZVector itself
 * uses two different levels of priorities (both lower than
 * the user lock priority).
 * level 1 is the lower priority and it's used just by the
 *         primitives in ZVector.
 * level 2 is the priority used by the ZVEctor functions that
 *         uses ZVEctor primitives.
 * level 3 is the priority of the User's locks.
 */
static inline zvect_retval check_mutex_lock(const vector v, const int32_t lock_type) {
	if (lock_enabled && lock_type >= v->lock_type) {
		mutex_lock(&(v->lock));
		v->lock_type = lock_type;
		return 1;
	}
	return 0;
}

static inline zvect_retval check_mutex_unlock(const vector v, const int32_t lock_type) {
	if (lock_enabled && lock_type == v->lock_type) {
		v->lock_type = 0;
		mutex_unlock(&(v->lock));
		return 1;
	}
	return 0;
}
#endif // ZVECT_THREAD_SAFE
/*---------------------------------------------------------------------------*/

/*****************************************************************************
 **                          ZVector Primitives                             **
 *****************************************************************************/

/*---------------------------------------------------------------------------*/
// Library Initialisation:

void p_init_zvect(void) {
#if (OS_TYPE == 1)
#	if ( !defined(macOS) )
		//mallopt(M_MXFAST, 196*sizeof(size_t)/4);
#	endif
#endif  // OS_TYPE == 1

	// We are done initialising ZVector so set the following
	// to one, so this function will no longer be called:
	p_init_state = 1;
}

/*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
// Vector's Utilities:

static inline zvect_retval p_vect_check(vector const x) {
	if (x == NULL)
		return ZVERR_VECTUNDEF;
	else
		return 0;
}

static inline zvect_index p_vect_capacity(vector const v) {
	return ( v->cap_left + v->cap_right );
}

static inline zvect_index p_vect_size(vector const v) {
	return ( v->end - v->begin );
}

static inline void p_item_safewipe(vector const v, void *const item) {
	if (item != NULL) {
		if (!(v->status & ZVS_CUST_WIPE_ON)) {
			memset(item, 0, v->data_size);
		} else {
			(*(v->SfWpFunc))(item, v->data_size);
		}
	}
}

static void p_free_items(vector const v, zvect_index first, zvect_index offset) {
	if (p_vect_size(v) == 0)
		return;

	for (register zvect_index j = (first + offset); j >= first; j--) {
		if (v->data[v->begin + j] != NULL) {
			if (v->flags & ZV_SEC_WIPE)
				p_item_safewipe(v, v->data[v->begin + j]);
			if (!(v->flags & ZV_BYREF)) {
				free(v->data[v->begin + j]);
				v->data[v->begin + j] = NULL;
			}
		}
		if (j == first)
			break;	// this is required if we are using
				// uint and the first element is element
				// 0, because on GCC an uint will fail
				// then check in the for loop if j >= first
				// in this particular case!
	}
}

/*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
// Vector Size and Capacity management functions:

/*
 * This function increase the CAPACITY of a vector.
 */
static zvect_retval p_vect_increase_capacity(vector const v, const zvect_index direction) {
	zvect_index new_capacity;

	void **new_data = NULL;
	zvect_index nb = 0;
	zvect_index ne = 0;
	if (!direction)
	{
		// Increase capacity on the left side of the vector:

		// Get actual left capacity and double it
		// Note: "<< 1" is the same as "* 2"
		//       this is an optimisation for old
		//       compilers.
		new_capacity = v->cap_left << 1;

		new_data = (void **)malloc(sizeof(void *) * (new_capacity + v->cap_right));
		if (new_data == NULL)
			return ZVERR_OUTOFMEM;

		nb = v->cap_left;
		ne = ( nb + (v->end - v->begin) );
		p_vect_memcpy(new_data + nb, v->data + v->begin, sizeof(void *) * (v->end - v->begin) );
	} else {
		// Increase capacity on the right side of the vector:

		// Get actual left capacity and double it
		// Note: "<< 1" is the same as "* 2"
		//       this is an optimisation for old
		//       compilers.
		new_capacity = v->cap_right << 1;

		new_data = (void **)realloc(v->data, sizeof(void *) * (v->cap_left + new_capacity));
		if (new_data == NULL)
			return ZVERR_OUTOFMEM;
	}

	// Apply changes and release memory
	if (!direction)
		free(v->data);
	v->data = new_data;
	if (direction == 0)
	{
		v->cap_left = new_capacity;
		v->end = ne;
		v->begin = nb;
	} else {
		v->cap_right = new_capacity;
	}

	// done
	return 0;
}

/*
 * This function decrease the CAPACITY of a vector.
 */
static zvect_retval p_vect_decrease_capacity(vector const v, const zvect_index direction) {
	// Check if new capacity is smaller than initial capacity
	if ( p_vect_capacity(v) <= v->init_capacity)
		return 0;

	zvect_index new_capacity;

	void **new_data = NULL;
	zvect_index nb = 0;
        zvect_index ne = 0;
	if (!direction)
	{
		// Decreasing on the left:
		// Note: ">> 1" is the same as "/ 2"
		//       this is an optimisation for old
		//       compilers.
		new_capacity = v->cap_left >> 1;

		if (new_capacity < ( v->init_capacity >> 1 ))
			new_capacity = v->init_capacity >> 1;

		new_capacity = max( (p_vect_size(v) >> 1), new_capacity);

		new_data = (void **)malloc(sizeof(void *) * (new_capacity + v->cap_right));
		if (new_data == NULL)
			return ZVERR_OUTOFMEM;

		nb = ( new_capacity >> 1);
		ne = ( nb + (v->end - v->begin) );
		p_vect_memcpy(new_data + nb, v->data + v->begin, sizeof(void *) * (v->end - v->begin) );
	} else {
		// Decreasing on the right:
		// Note: ">> 1" is the same as "/ 2"
		//       this is an optimisation for old
		//       compilers.
		new_capacity = v->cap_right >> 1;

		if (new_capacity < ( v->init_capacity >> 1 ))
			new_capacity = v->init_capacity >> 1;

		new_capacity = max( (p_vect_size(v) >> 1), new_capacity);

		new_data = (void **)realloc(v->data, sizeof(void *) * (v->cap_left + new_capacity));
		if (new_data == NULL)
			return ZVERR_OUTOFMEM;
	}

	// Apply changes and release memory:
	if (!direction)
		free(v->data);
	v->data = new_data;

	if (direction == 0)
	{
		v->cap_left = new_capacity;
		v->end = ne;
		v->begin = nb;
	} else {
		v->cap_right = new_capacity;
	}

	// done:
	return 0;
}

/*
 * This function shrinks the CAPACITY of a vector
 * not its size. To reduce the size of a vector we
 * need to remove items from it.
 */
static zvect_retval p_vect_shrink(vector const v) {
	if (v->init_capacity < 2)
		v->init_capacity = 2;

	if (p_vect_capacity(v) == v->init_capacity || p_vect_capacity(v) <= p_vect_size(v))
		return 0;

	// Determine the correct shrunk size:
	zvect_index new_capacity;
	if (p_vect_size(v) < v->init_capacity)
		new_capacity = v->init_capacity;
	else
		new_capacity = p_vect_size(v) + 2;

	// shrink the vector:
	// Given that zvector supports vectors that can grow on the left and on the right
	// I cannot use realloc here.
	void **new_data = (void **)malloc(sizeof(void *) * new_capacity);
	if (new_data == NULL)
		return ZVERR_OUTOFMEM;

	zvect_index ne;
	zvect_index nb;
	// Note: ">> 1" is the same as "/ 2"
	//       this is an optimisation for old
	//       compilers.
	nb = ( new_capacity >> 1 );
	ne = ( nb + (v->end - v->begin) );
	p_vect_memcpy(new_data + nb, v->data + v->begin, sizeof(void *) * (v->end - v->begin) );

	// Apply changes:
	free(v->data);
	v->data = new_data;
	v->end = ne;
	v->begin = nb;
	v->cap_left = new_capacity >> 1;
	v->cap_right = new_capacity >> 1;

	// done:
	return 0;
}

/*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
// Vector data storage primitives:

// inline implementation for all put:
static inline zvect_retval p_vect_put_at(vector const v, const void *value,
                                const zvect_index i) {
	// Check if the index passed is out of bounds:
	zvect_index idx = i;
	if (!(v->flags & ZV_CIRCULAR))
	{
		if (idx >= p_vect_size(v))
			return ZVERR_IDXOUTOFBOUND;
	} else {
		if (idx >= p_vect_size(v))
			idx = i % v->init_capacity;
	}

	// Add value at the specified index, considering
	// if the vector has ZV_BYREF property enabled:
	if ( v->flags & ZV_BYREF ) {
		void *temp = v->data[v->begin + idx];
		v->data[v->begin + idx] = (void *)value;
		if ( v->flags & ZV_SEC_WIPE )
			memset((void *)temp, 0, v->data_size);
	} else {
		p_vect_memcpy(v->data[v->begin + idx], value, v->data_size);
	}

	// done
	return 0;
}

// inline implementation for all add(s):
static inline zvect_retval p_vect_add_at(vector const v, const void *value,
                                const zvect_index i, const int32_t action) {
	// If the vector is circular then use vect_put_at
	// instead:
	if (v->flags & ZV_CIRCULAR)
		return p_vect_put_at(v, value, i);

	zvect_index idx = i;

	// Get vector size:
	zvect_index vsize = p_vect_size(v);

	// Check if the provided index is out of bounds:
	if (idx > vsize)
	{
		if (action == 0)
			return ZVERR_IDXOUTOFBOUND;
		else
			idx = vsize - 1;
	}

#if (ZVECT_FULL_REENTRANT == 1)
	// If we are in FULL_REENTRANT MODE prepare for potential
	// array copy:
	void **new_data = NULL;
	if (idx < vsize) {
		new_data = (void **)malloc(sizeof(void *) * p_vect_capacity(v));
		if (new_data == NULL)
			return ZVERR_OUTOFMEM;
	}
#endif

	// Check if we need to expand the vector:
	if (!idx) {
		// Check if we need to expand on the left side:
		if ( v->begin == 0 || v->cap_left == 1 )
			p_vect_increase_capacity(v, 0);
	} else {
		// Check if we need to expand on thr right side:
		if ( v->end >= v->cap_right )
			p_vect_increase_capacity(v, 1);
	}

	// Allocate memory for the new item:
	zvect_index base = v->begin;
	if (!idx) {
		// Prepare left side of the vector:
		base--;
		if (!(v->flags & ZV_BYREF)) {
			v->data[base] = (void *)malloc(v->data_size);
			if (v->data[base] == NULL)
				return ZVERR_OUTOFMEM;
		}
	} else if (idx == vsize && !(v->flags & ZV_BYREF)) {
		// Prepare right side of the vector:
		v->data[base + vsize] = (void *)malloc(v->data_size);
		if (v->data[base + vsize] == NULL)
			return ZVERR_OUTOFMEM;
	}

	// "Shift" right the array of one position to make space for the new item:
	int16_t array_changed = 0;
	if ((idx < vsize) && (idx != 0)) {
		array_changed = 1;
#if (ZVECT_FULL_REENTRANT == 1)
		// Algorithm to try to copy an array of pointers as fast as possible:
		if (idx > 0)
			p_vect_memcpy(new_data + base, v->data + base, sizeof(void *) * idx);
		p_vect_memcpy(new_data + base + (idx + 1), v->data + base + idx,
			    sizeof(void *) * (p_vect_size(v) - idx));
#else
		// We can't use the vect_memcpy when not in full reentrant code
		// because it's not safe to use it on the same src and dst.
		vect_memmove(v->data + base + (idx + 1), v->data + base + idx,
			     sizeof(void *) * (vsize - idx));
#endif  // (ZVECT_FULL_REENTRANT == 1)
	}

	// Add new value in (at the index i):
#if (ZVECT_FULL_REENTRANT == 1)
	if (array_changed) {
		if (v->flags & ZV_BYREF) {
			new_data[base + idx] = (void *)value;
		} else {
			new_data[base + idx] = (void *)malloc(v->data_size);
			if (new_data[base + idx] == NULL)
				return ZVERR_OUTOFMEM;
			p_vect_memcpy(new_data[base + idx], value, v->data_size);
		}
	} else {
		if (v->flags & ZV_BYREF)
			v->data[base + idx] = (void *)value;
		else
			p_vect_memcpy(v->data[base + idx], value, v->data_size);
	}
#else
	if (array_changed && !(v->flags & ZV_BYREF)) {
		// We moved chunks of memory, so we need to
		// allocate new memory for the item at position i:
		v->data[base + idx] = malloc(v->data_size);
		if (v->data[base + idx] == NULL)
			return ZVERR_OUTOFMEM;
	}
	if (v->flags & ZV_BYREF)
		v->data[base + idx] = (void *)value;
	else
		p_vect_memcpy(v->data[base + idx], value, v->data_size);
#endif  // (ZVECT_FULL_REENTRANT == 1)

	// Apply changes:
#if (ZVECT_FULL_REENTRANT == 1)
	if (array_changed) {
		free(v->data);
		v->data = new_data;
	}
#endif
	// Increment vector size
	v->prev_end = vsize;
	if (!idx)
		v->begin = base;
	else
		v->end++;

	// done
	return 0;

#if (ZVECT_FULL_REENTRANT == 1)
	UNUSED(new_data);
#endif
}

// This is the inline implementation for all the remove and pop
static inline zvect_retval p_vect_remove_at(vector const v, const zvect_index i, const int32_t action, void **item) {
	zvect_index idx = i;

	// Get the vector size:
	zvect_index vsize = p_vect_size(v);

	// If the vector is empty just return null
	if (vsize == 0)
		return 0;

	// Check if the index is out of bounds:
	if (!(v->flags & ZV_CIRCULAR))
	{
		if (idx >= vsize)
		{
			if (action == 0)
				return ZVERR_IDXOUTOFBOUND;
			else
				idx = vsize - 1;
		}
	} else {
		if (idx >= vsize)
			idx = idx % vsize;
	}

	// Check if the vector got corrupted
	if (v->begin > v->end)
		return ZVERR_VECTCORRUPTED;

	// Start processing the vector:
#if (ZVECT_FULL_REENTRANT == 1)
	// Allocate memory for support Data Structure:
	void **new_data = (void **)malloc(sizeof(void *) * p_vect_capacity(v));
	if (new_data == NULL)
		return ZVERR_OUTOFMEM;
#endif

	// Get the value we are about to remove:
	// If the vector is set as ZV_BYREF, then just copy the pointer to the item
	// If the vector is set as regular, then copy the item
	zvect_index base = v->begin;
	if (v->flags & ZV_BYREF) {
		*item = v->data[base + idx];
	} else {
		*item = (void **)malloc(v->data_size);
		p_vect_memcpy(*item, v->data[base + idx], v->data_size);
		// If the vector is set for secure wipe, and we copied the item
		// then we need to wipe the old copy:
		if (v->flags & ZV_SEC_WIPE)
			p_item_safewipe(v, v->data[base + idx]);
	}

	// "shift" left the array of one position:
	uint16_t array_changed = 0;
	if ( idx != 0 ) {
		if ((idx < (vsize - 1)) && (vsize > 0)) {
			array_changed = 1;
			free(v->data[base + idx]);
#if (ZVECT_FULL_REENTRANT == 1)
			p_vect_memcpy(new_data + base, v->data + base, sizeof(void *) * idx);
			p_vect_memcpy(new_data + base + idx, v->data + base + (idx + 1),
				sizeof(void *) * (vsize - idx));
#else
			// We can't use the vect_memcpy when not in full reentrant code
			// because it's not safe to use it on the same src and dst.
			vect_memmove(v->data + base + idx, v->data + base + (idx + 1),
				sizeof(void *) * (vsize - idx));
#endif
		}
	} else {
		if ( base < v->end ) {
			array_changed = 1;
			if (v->data[base] != NULL)
				free(v->data[base]);
		}
	}

	// Reduce vector size:
#if (ZVECT_FULL_REENTRANT == 0)
	if (!(v->flags & ZV_BYREF)) {
		if (!array_changed)
			p_free_items(v, vsize - 1, 0);
	}
#else
	// Apply changes
	if (array_changed) {
		free(v->data);
		v->data = new_data;
	}
#endif
	if (!(v->flags & ZV_CIRCULAR)) {
		v->prev_end = vsize;
		if ( idx != 0 ) {
			if (v->end > v->begin) {
				v->end--;
			} else {
				v->end = v->begin;
			}
		} else {
			if (v->begin < v->end) {
				v->begin++;
			} else {
				v->begin = v->end;
			}
		}
		// Check if we need to shrink vector's capacity:
		if ((4 * vsize) < p_vect_capacity(v) )
			p_vect_decrease_capacity(v, idx);
	}

	// All done, return control:
	return 0;
}

// This is the inline implementation for all the "delete" methods
static inline zvect_retval p_vect_delete_at(vector const v, const zvect_index start,
                                   	    const zvect_index offset) {

	zvect_index vsize = p_vect_size(v);

	// Check if the index is out of bounds:
	if ((start + offset) >= vsize)
		return ZVERR_IDXOUTOFBOUND;

	// If the vector is empty just return
	if (vsize == 0)
		return ZVERR_VECTEMPTY;

	uint16_t array_changed = 0;

	// "shift" left the data of one position:
	zvect_index tot_items = start + offset;
	if ( tot_items < (vsize - 1) ) {
		array_changed = 1;
		p_free_items(v, start, offset);
		if ( tot_items )
			vect_memmove(v->data + (v->begin + start), v->data + (v->begin + (tot_items + 1)),
				sizeof(void *) * (vsize - start));
	}

	// Reduce vector size:
	if (!(v->flags & ZV_BYREF)) {
		if (!array_changed)
			p_free_items(v, ((vsize - 1) - offset), offset);
	}
	v->prev_end = vsize;
	if ( start != 0 ) {
		if ((v->end - (offset + 1)) > v->begin) {
			v->end -= (offset + 1);
		} else {
			v->end = v->begin;
		}
	} else {
		if ((v->begin + (offset + 1)) < v->end) {
			v->begin += (offset + 1);
		} else {
			v->begin = v->end;
		}
	}

	// Check if we need to shrink the vector:
	if ((4 * vsize) < p_vect_capacity(v))
		p_vect_decrease_capacity(v, start);

	// All done, return control:
	return 0;
}

/*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
// Creation and destruction primitives:

static zvect_retval p_vect_destroy(vector v, uint32_t flags) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
	if (!lock_owner && lock_enabled)
		return ZVERR_RACECOND;
#endif
	// Clear the vector:
	if ((p_vect_size(v) > 0) && (flags & 1)) {
		// Secure Wipe the vector (or just free) depending on vector properties:
		zvect_index i = p_vect_size(v); // if p_vect_size(v) is 200, then the first "i" below will be 199
		while (i--) {
			if (v->data[v->begin + i] != NULL) {
				if ((v->flags & ZV_SEC_WIPE))
					p_item_safewipe(v, v->data[v->begin + i]);

				if (!(v->flags & ZV_BYREF)) {
					free(v->data[v->begin + i]);
					v->data[v->begin + i] = NULL;
				}
			}
		}
	}

	// Reset interested descriptors:
	v->prev_end = p_vect_size(v);
	v->end = 0;

	// Shrink Vector's capacity:
	if (p_vect_capacity(v) > v->init_capacity)
		p_vect_shrink(v);

	v->prev_end = 0;
	v->init_capacity = 0;
	v->cap_left = 0;
	v->cap_right = 0;

	// Destroy it:
	if ((v->status & ZVS_CUST_WIPE_ON)) {
		//free(v->SfWpFunc);
		v->SfWpFunc = NULL;
	}

	if (v->data != NULL) {
		free(v->data);
		v->data = NULL;
	}

	// Clear vector status flags:
	v->status = 0;
	v->flags = 0;
	v->begin = 0;
	v->end = 0;
	v->data_size = 0;
	v->balance = 0;
	v->bottom = 0;

#if (ZVECT_THREAD_SAFE == 1)
	check_mutex_unlock(v, 1);
	mutex_destroy(&(v->lock));
#endif

	// All done and freed, so we can safely
	// free the vector itself:
	free(v);
	v = NULL;

	return 0;
}

/*---------------------------------------------------------------------------*/

/*****************************************************************************
 **                            ZVector API                                  **
 *****************************************************************************/

/*---------------------------------------------------------------------------*/
// Vector Size and Capacity management functions:

/*
 * Public method to request ZVector to
 * shrink a vector.
 */
void vect_shrink(vector const v) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif

	zvect_retval rval = p_vect_shrink(v);

#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if (rval)
		p_throw_error(rval, NULL);
}

/*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
// Vector Structural Information report:

bool vect_is_empty(vector const v) {
	// Check if the vector exists
	if (!p_vect_check(v))
		return p_vect_size(v) == 0;
	return ZVERR_VECTUNDEF;
}

zvect_index vect_size(vector const v) {
	// Check if the vector exists
	if (!p_vect_check(v))
		return p_vect_size(v);
	return 0;
}

zvect_index vect_max_size(vector const v) {
	// Check if the vector exists
	if (!p_vect_check(v))
		return zvect_index_max;
	return 0;
}

void *vect_begin(vector const v) {
	// Check if the vector exists
	if (!p_vect_check(v))
		return v->data[v->begin];
	return NULL;
}

void *vect_end(vector const v) {
	// Check if the vector exists
	if (!p_vect_check(v))
		return v->data[v->end];
	return NULL;
}

/*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
// Vector Creation and Destruction:

vector vect_create(const size_t init_capacity, const size_t item_size,
                   const uint32_t properties) {
	// If ZVector has not been initialised yet, then initialise it
	// when creating the first vector:
	if (p_init_state == 0)
		p_init_zvect();

	// Create the vector first:
	vector v = (vector)malloc(sizeof(struct p_vector));
	if (v == NULL)
		p_throw_error(ZVERR_OUTOFMEM, NULL);

	// Initialize the vector:
	v->prev_end = 0;
	v->end = 0;
	if (item_size == 0)
		v->data_size = ZVECT_DEFAULT_DATA_SIZE;
	else
		v->data_size = item_size;

	zvect_index capacity = init_capacity;
	if (init_capacity == 0)
	{
		v->cap_left = ZVECT_INITIAL_CAPACITY / 2;
		v->cap_right= ZVECT_INITIAL_CAPACITY / 2;
	} else {

		if (init_capacity <= 4)
			capacity = 4;

		v->cap_left = capacity / 2;
		v->cap_right= capacity / 2;
	}
	v->begin = 1;
	v->end = 1;

	v->init_capacity = v->cap_left + v->cap_right;
	v->flags = properties;
	v->SfWpFunc = NULL;
	v->status = 0;
	if (v->flags & ZV_CIRCULAR)
	{
		// If the vector is circular then
		// we need to pre-allocate all the elements
		// the vector will not grow:
		v->end = v->cap_right - 1;
		v->begin = v->cap_left - 1;
	}
#ifdef ZVECT_DMF_EXTENSIONS
	v->balance = 0;
	v->bottom = 0;
#endif // ZVECT_DMF_EXTENSIONS

	v->data = NULL;

#if (ZVECT_THREAD_SAFE == 1)
	v->lock_type = 0;
	mutex_alloc(&(v->lock));
#endif

	// Allocate memory for the vector storage area
	v->data = (void **)calloc(p_vect_capacity(v), sizeof(void *));
	if (v->data == NULL)
		p_throw_error(ZVERR_OUTOFMEM, NULL);

	// Return the vector to the user:
	return v;
}

void vect_destroy(vector v) {
	// Call p_vect_destroy with flags set to 1
	// to destroy data according to the vector
	// properties:
	zvect_retval rval = p_vect_check(v);
	if (!rval)
		rval = p_vect_destroy(v, 1);

	if (rval)
		p_throw_error(rval, NULL);
}

/*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
// Vector Thread Safe user functions:

#if (ZVECT_THREAD_SAFE == 1)
void vect_lock_enable(void) {
	lock_enabled = true;
}

void vect_lock_disable(void) {
	lock_enabled = false;
}

inline void vect_lock(vector const v) {
	check_mutex_lock(v, 3);
}

inline void vect_unlock(vector const v) {
	check_mutex_unlock(v, 3);
}
#endif

/*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
// Vector Data Storage functions:

void vect_clear(vector const v) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif

	// check if the vector exists:
	zvect_retval rval = p_vect_check(v);
	if (rval)
		goto DONE_PROCESSING;

	// Clear the vector:
	if (!vect_is_empty(v))
		p_free_items(v, 0, (p_vect_size(v) - 1));

	// Reset interested descriptors:
	v->prev_end = p_vect_size(v);
	v->begin = 1;
	v->end = 1;

	// Shrink Vector's capacity:
	// p_vect_shrink(v); //commented this out to make vect_clear behave more like the clear method in C++

	// Done.
DONE_PROCESSING:
#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if (rval)
		p_throw_error(rval, NULL);
}

void vect_set_wipefunct(const vector v, void (*f1)(const void *, size_t)) {
	v->SfWpFunc = (void *)malloc(sizeof(void *));
	if (v->SfWpFunc == NULL)
		p_throw_error(ZVERR_OUTOFMEM, NULL);

	// Set custom Safe Wipe function:
	v->SfWpFunc = f1;
	// p_vect_memcpy(v->SfWpFunc, f1, sizeof(void *));
	v->status |= ZVS_CUST_WIPE_ON;
}

// Add an item at the END (top) of the vector
inline void vect_push(const vector v, const void *value) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif

	zvect_retval rval = p_vect_check(v);
	if (!rval)
		rval = p_vect_add_at(v, value, p_vect_size(v), -1);

#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if (rval)
		p_throw_error(rval, NULL);
}

// Add an item at the END of the vector
void vect_add(const vector v, const void *value) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif

	zvect_retval rval = p_vect_check(v);
	if (!rval)
		rval = p_vect_add_at(v, value, p_vect_size(v), -1);

#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if (rval)
		p_throw_error(rval, NULL);
}

// Add an item at position "i" of the vector
void vect_add_at(const vector v, const void *value, const zvect_index i) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif

	zvect_retval rval = p_vect_check(v);
	if (!rval)
		rval = p_vect_add_at(v, value, i, 0);

#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if (rval)
		p_throw_error(rval, NULL);
}

// Add an item at the FRONT of the vector
void vect_add_front(vector const v, const void *value) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif

	zvect_retval rval = p_vect_check(v);
	if (!rval)
		rval = p_vect_add_at(v, value, 0, -1);

#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if (rval)
		p_throw_error(rval, NULL);
}

// inline implementation for all get(s):
static inline void *p_vect_get_at(vector const v, const zvect_index i) {
	// Check if passed index is out of bounds:
	if (i >= p_vect_size(v))
		p_throw_error(ZVERR_IDXOUTOFBOUND, NULL);

	// Return found element:
	return v->data[v->begin + i];
}

void *vect_get(vector const v) {
	// check if the vector exists:
	zvect_retval rval = p_vect_check(v);
	if (!rval)
		return p_vect_get_at(v, p_vect_size(v) - 1);
	else
		p_throw_error(rval, NULL);

	return NULL;
}

void *vect_get_at(vector const v, const zvect_index i) {
	// check if the vector exists:
	zvect_retval rval = p_vect_check(v);
	if (!rval)
		return p_vect_get_at(v, i);
	else
		p_throw_error(rval, NULL);

	return NULL;
}

void *vect_get_front(vector const v) {
	// check if the vector exists:
	zvect_retval rval = p_vect_check(v);
	if (!rval)
		return p_vect_get_at(v, 0);
	else
		p_throw_error(rval, NULL);

	return NULL;
}

void vect_put(vector const v, const void *value) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif

	zvect_retval rval = p_vect_check(v);
	if (!rval)
		rval = p_vect_put_at(v, value, p_vect_size(v) - 1);

#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if (rval)
		p_throw_error(rval, NULL);
}

void vect_put_at(vector const v, const void *value, const zvect_index i) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif

	zvect_retval rval = p_vect_check(v);
	if (!rval)
		rval = p_vect_put_at(v, value, i);

#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if (rval)
		p_throw_error(rval, NULL);
}

void vect_put_front(vector const v, const void *value) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif

	zvect_retval rval = p_vect_check(v);
	if (!rval)
		rval = p_vect_put_at(v, value, 0);

#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if (rval)
		p_throw_error(rval, NULL);
}

inline void *vect_pop(vector const v) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif

	void *item = NULL;
	zvect_retval rval = p_vect_check(v);
	if (!rval)
		rval = p_vect_remove_at(v, p_vect_size(v) - 1, -1, &item);

#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if (rval)
		p_throw_error(rval, NULL);

	return item;
}

void *vect_remove(vector const v) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif

	void *item = NULL;
	zvect_retval rval = p_vect_check(v);
	if (!rval)
		rval = p_vect_remove_at(v, p_vect_size(v) - 1, -1, &item);

#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if (rval)
		p_throw_error(rval, NULL);

	return item;
}

void *vect_remove_at(vector const v, const zvect_index i) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif

	void *item = NULL;
	zvect_retval rval = p_vect_check(v);
	if (!rval)
		rval = p_vect_remove_at(v, i, 0, &item);

#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if (rval)
		p_throw_error(rval, NULL);

	return item;
}

void *vect_remove_front(vector const v) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif

	void *item = NULL;
	zvect_retval rval = p_vect_check(v);
	if (!rval)
		rval = p_vect_remove_at(v, 0, 0, &item);

#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if (rval)
		p_throw_error(rval, NULL);

	return item;
}

// Delete an item at the END of the vector
void vect_delete(vector const v) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif

	zvect_retval rval = p_vect_check(v);
	if (!rval)
		rval = p_vect_delete_at(v, p_vect_size(v) - 1, 0);

#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if (rval)
		p_throw_error(rval, NULL);
}

// Delete an item at position "i" on the vector
void vect_delete_at(const vector v, const zvect_index i) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif

	zvect_retval rval = p_vect_check(v);
	if (!rval)
		rval = p_vect_delete_at(v, i, 0);

#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if (rval)
		p_throw_error(rval, NULL);
}

// Delete a range of items from "first_element" to "last_element" on the vector v
void vect_delete_range(vector const v, const zvect_index first_element,
                       const zvect_index last_element) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif

	zvect_retval rval = p_vect_check(v);
	if (!rval) {
		zvect_index end = (last_element - first_element);
		rval = p_vect_delete_at(v, first_element, end);
	}

#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if (rval)
		p_throw_error(rval, NULL);
}

// Delete an item at the BEGINNING of a vector v
void vect_delete_front(vector const v) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif

	zvect_retval rval = p_vect_check(v);
	if (!rval)
		rval = p_vect_delete_at(v, 0, 0);

#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if (rval)
		p_throw_error(rval, NULL);
}

/*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
// Vector Data Manipulation functions
#ifdef ZVECT_DMF_EXTENSIONS

void vect_swap(vector const v, const zvect_index i1, const zvect_index i2) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif
	// check if the vector exists:
	zvect_retval rval = p_vect_check(v);
	if (rval)
		goto DONE_PROCESSING;

	// Check parameters:
	if (i1 > p_vect_size(v) || i2 > p_vect_size(v)) {
		rval = ZVERR_IDXOUTOFBOUND;
		goto DONE_PROCESSING;
	}

	if (i1 == i2)
		goto DONE_PROCESSING;

	// Let's swap items:
	void *temp = v->data[v->begin + i2];
	v->data[v->begin + i2] = v->data[v->begin + i1];
	v->data[v->begin + i1] = temp;

DONE_PROCESSING:
#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if (rval)
		p_throw_error(rval, NULL);
}

void vect_swap_range(vector const v, const zvect_index s1, const zvect_index e1,
                     const zvect_index s2) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif

	// check if the vector exists:
	zvect_retval rval = p_vect_check(v);
	if (rval)
		goto DONE_PROCESSING;

	// Check parameters:
	zvect_index end = e1;
	if (e1 != 0)
		end = e1 - s1;

	if ((s1 + end) > p_vect_size(v) || (s2 + end) > p_vect_size(v) || s2 < (s1 + end)) {
		rval = ZVERR_IDXOUTOFBOUND;
		goto DONE_PROCESSING;
	}

	if (s1 == s2)
		goto DONE_PROCESSING;

	// Let's swap items:
	register zvect_index i;
	for (register zvect_index j = s1; j <= (s1 + end); j++) {
		i = j - s1;
		void *temp = v->data[v->begin + j];
		v->data[v->begin + j] = v->data[v->begin + (s2 + i)];
		v->data[v->begin + (s2 + i)] = temp;
	}

DONE_PROCESSING:
#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if (rval)
		p_throw_error(rval, NULL);
}

void vect_rotate_left(vector const v, const zvect_index i) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif
	// check if the vector exists:
	zvect_retval rval = p_vect_check(v);
	if (rval)
		goto DONE_PROCESSING;

	// Check parameters:
	if (i == 0 || i == p_vect_size(v))
		goto DONE_PROCESSING;

	if (i > p_vect_size(v)) {
		rval = ZVERR_IDXOUTOFBOUND;
		goto DONE_PROCESSING;
	}

	// Process the vector
	if (i == 1) {
		// Rotate left the vector of 1 position:
		void *temp = v->data[0];
		vect_memmove(v->data, v->data + 1, sizeof(void *) * (p_vect_size(v) - 1));
		v->data[p_vect_size(v) - 1] = temp;
	} else {
		void **new_data = (void **)malloc(sizeof(void *) * p_vect_capacity(v));
		if (new_data == NULL) {
			rval = ZVERR_OUTOFMEM;
			goto DONE_PROCESSING;
		}
		// Rotate left the vector of "i" positions:
		p_vect_memcpy(new_data + v->begin, v->data + v->begin, sizeof(void *) * i);
		vect_memmove(v->data + v->begin, v->data + v->begin + i, sizeof(void *) * (p_vect_size(v) - i));
		p_vect_memcpy(v->data + v->begin + (p_vect_size(v) - i), new_data + v->begin, sizeof(void *) * i);

		free(new_data);
	}

DONE_PROCESSING:
#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if (rval)
		p_throw_error(rval, NULL);
}

void vect_rotate_right(vector const v, const zvect_index i) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif
	// check if the vector exists:
	zvect_retval rval = p_vect_check(v);
	if (rval)
		goto DONE_PROCESSING;

	// Check parameters:
	if (i == 0 || i == p_vect_size(v))
		goto DONE_PROCESSING;

	if (i > p_vect_size(v)) {
		rval = ZVERR_IDXOUTOFBOUND;
		goto DONE_PROCESSING;
	}

	// Process the vector
	if (i == 1) {
		// Rotate right the vector of 1 position:
		void *temp = v->data[v->begin + p_vect_size(v) - 1];
		vect_memmove(v->data + v->begin + 1, v->data + v->begin, sizeof(void *) * (p_vect_size(v) - 1));
		v->data[v->begin] = temp;
	} else {
		void **new_data = (void **)malloc(sizeof(void *) * p_vect_capacity(v));
		if (new_data == NULL) {
			rval = ZVERR_OUTOFMEM;
			goto DONE_PROCESSING;
		}

		// Rotate right the vector of "i" positions:
		p_vect_memcpy(new_data + v->begin, v->data + v->begin + (p_vect_size(v) - i), sizeof(void *) * i);
		vect_memmove(v->data + v->begin + i, v->data + v->begin, sizeof(void *) * (p_vect_size(v) - i));
		p_vect_memcpy(v->data + v->begin, new_data + v->begin, sizeof(void *) * i);

		free(new_data);
	}

DONE_PROCESSING:
#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if (rval)
		p_throw_error(rval, NULL);
}

#ifdef TRADITIONAL_QSORT
static inline zvect_index
p_partition(vector v, zvect_index low, zvect_index high,
           int (*compare_func)(const void *, const void *)) {
	if (high >= p_vect_size(v))
		high = p_vect_size(v) - 1;

	void *pivot = v->data[v->begin + high];
	zvect_index i = (low - 1);

	for (register zvect_index j = low; j <= (high - 1); j++) {
		// v->data[j] <= pivot
		if ((*compare_func)(v->data[v->begin + j], pivot) <= 0)
			vect_swap(v, ++i, j);
	}

	vect_swap(v, i + 1, high);
	return (i + 1);
}

static void p_vect_qsort(vector v, zvect_index low, zvect_index high,
                        int (*compare_func)(const void *, const void *)) {
	if (low < high) {
		zvect_index pi = p_partition(v, low, high, compare_func);
		p_vect_qsort(v, low, pi - 1, compare_func);
		p_vect_qsort(v, pi + 1, high, compare_func);
	}
}
#endif // TRADITIONAL_QSORT

#ifndef TRADITIONAL_QSORT
// This is my much faster implementation of a quicksort algorithm
// it fundamentally uses the 3 ways partitioning adapted and improved
// to deal with arrays of pointers together with having a custom
// compare function:
static void p_vect_qsort(const vector v, zvect_index l, zvect_index r,
                        int (*compare_func)(const void *, const void *)) {
	if (r <= l)
	    return;

	zvect_index i, p, j, q;
	void *ref_val = NULL;

	// l = left (also low)
	if (l > 0)
		i = l - 1;
	else
		i = l;
	// r = right (also high)
	j = r;
	p = i;
	q = r;
	ref_val = v->data[v->begin + r];

	for (;;) {
		while ((*compare_func)(v->data[v->begin + i], ref_val) < 0)
			i++;
		while ((*compare_func)(ref_val, v->data[(--j) + v->begin]) < 0)
			if (j == l)
				break;
		if (i >= j)
			break;
		vect_swap(v, i, j);
		if ((*compare_func)(v->data[v->begin + i], ref_val) == 0) {
			p++;
			vect_swap(v, p, i);
		}
		if ((*compare_func)(ref_val, v->data[v->begin + j]) == 0) {
			q--;
			vect_swap(v, q, j);
		}
	}
	vect_swap(v, i, r);
	j = i - 1;
	i = i + 1;
    	register zvect_index k;
	for (k = l; k < p; k++, j--)
		vect_swap(v, k, j);
	for (k = r - 1; k > q; k--, i++)
		vect_swap(v, k, i);
	p_vect_qsort(v, l, j, compare_func);
	p_vect_qsort(v, i, r, compare_func);
}
#endif // ! TRADITIONAL_QSORT

void vect_qsort(const vector v, int (*compare_func)(const void *, const void *)) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif
	// check if the vector v1 exists:
	zvect_retval rval = p_vect_check(v);
	if (rval)
		goto DONE_PROCESSING;

	// Check parameters:
	if (p_vect_size(v) <= 1 || compare_func == NULL)
		goto DONE_PROCESSING;

	// Process the vector:
	p_vect_qsort(v, 0, p_vect_size(v) - 1, compare_func);

DONE_PROCESSING:
#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if (rval)
		p_throw_error(rval, NULL);
}

#ifdef TRADITIONAL_BINARY_SEARCH
static bool p_standard_binary_search(vector v, const void *key,
                                    zvect_index *item_index,
                                    int (*f1)(const void *, const void *)) {
	zvect_index bot, top;

	bot = 0;
	top = p_vect_size(v) - 1;

	while (bot < top) {
        zvect_index mid // mid-point
		mid = top - (top - bot) / 2;

		// key < array[mid]
		if ((*f1)(key, v->data[v->begin + mid]) < 0) {
			top = mid - 1;
		} else {
			bot = mid;
		}
	}

	// key == array[top]
	if ((*f1)(key, v->data[v->begin + top]) == 0) {
		*item_index = top;
		return true;
	}

	*item_index = top;
	return false;
}
#endif // TRADITIONAL_BINARY_SEARCH

#ifndef TRADITIONAL_BINARY_SEARCH
// This is my re-implementation of Igor van den Hoven's Adaptive
// Binary Search algorithm. It has few improvements over the
// original design, most notably the use of custom compare
// function that makes it suitable also to search through strings
// and other types of vectors.
static bool p_adaptive_binary_search(vector const v, const void *key,
                                    zvect_index *item_index,
                                    int (*f1)(const void *, const void *)) {
	zvect_index bot, top, mid;

	if ((v->balance >= 32) || (p_vect_size(v) <= 64)) {
		bot = 0;
		top = p_vect_size(v);
		goto monobound;
	}
	bot = v->bottom;
	top = 32;

	// key >= array[bot]
	if ((*f1)(key, v->data[v->begin + bot]) >= 0) {
		while (1) {
			if ((bot + top) >= p_vect_size(v)) {
				top = p_vect_size(v) - bot;
				break;
			}
			bot += top;

			// key < array[bot]
			if ((*f1)(key, v->data[v->begin + bot]) < 0) {
				bot -= top;
				break;
			}
			top *= 2;
		}
	} else {
		while (1) {
			if (bot < top) {
				top = bot;
				bot = 0;
				break;
			}
			bot -= top;

			// key >= array[bot]
			if ((*f1)(key, v->data[v->begin + bot]) >= 0)
				break;
			top *= 2;
		}
	}

	monobound:
	while (top > 3) {
		mid = top / 2;
		// key >= array[bot + mid]
		if ((*f1)(key, v->data[v->begin + (bot + mid)]) >= 0)
			bot += mid;
		top -= mid;
	}

	v->balance = v->bottom > bot ? v->bottom - bot : bot - v->bottom;
	v->bottom = bot;

	while (top) {
		// key == array[bot + --top]
		int test = (*f1)(key, v->data[v->begin + (bot + (--top))]);
		if (test == 0) {
			*item_index = bot + top;
			return true;
		} else if (test > 0) {
			*item_index = bot + (top + 1);
			return false;
		}
	}

	*item_index = bot + top;
	return false;
}
#endif // ! TRADITIONAL_BINARY_SEARCH

bool vect_bsearch(vector const v, const void *key,
                  int (*f1)(const void *, const void *),
                  zvect_index *item_index) {
	// check if the vector exists:
	zvect_retval rval = p_vect_check(v);
	if (rval)
		goto DONE_PROCESSING;

	// Check parameters:
	if (key == NULL || f1 == NULL || p_vect_size(v) == 0)
		goto DONE_PROCESSING;

	*item_index = 0;
#ifdef TRADITIONAL_BINARY_SEARCH
	if (p_standard_binary_search(v, key, item_index, f1)) {
		return true;
	} else {
		*item_index = 0;
		return false;
	}
#endif // TRADITIONAL_BINARY_SEARCH
#ifndef TRADITIONAL_BINARY_SEARCH
	if (p_adaptive_binary_search(v, key, item_index, f1)) {
		return true;
	} else {
		*item_index = 0;
		return false;
	}
#endif // ! TRADITIONAL_BINARY_SEARCH

DONE_PROCESSING:
	if (rval)
		p_throw_error(rval, NULL);

	return false;
}

/*
 * Although if the vect_add_* doesn't belong to this group of
 * functions, the vect_add_ordered is an exception because it
 * requires vect_bserach and vect_qsort to be available.
 */
void vect_add_ordered(vector const v, const void *value,
                      int (*f1)(const void *, const void *)) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 2);
#endif
	// check if the vector exists:
	zvect_retval rval = p_vect_check(v);
	if (rval)
		goto DONE_PROCESSING;

	// Check parameters:
	if (value == NULL)
		goto DONE_PROCESSING;

	// Few tricks to make it faster:
	if (p_vect_size(v) == 0) {
		// If the vector is empty clearly we can just
		// use vect_add and add the value normally!
		vect_add(v, value);
		goto DONE_PROCESSING;
	}

	if ((*f1)(value, v->data[v->begin + (p_vect_size(v) - 1)]) > 0) {
		// If the compare function returns that
		// the value passed should go after the
		// last value in the vector, just do so!
		vect_add(v, value);
		goto DONE_PROCESSING;
	}

	// Ok previous checks didn't help us, so we need
	// to get "heavy weapons" out and find where in
	// the vector we should add "value":
	zvect_index item_index = 0;

	// Here is another trick:
	// I improved adaptive binary search to ALWAYS
	// return an index (even when it doesn't find a
	// searched item), this works for both: regular
	// searches which will also use the bool to
	// know if we actually found the item in that
	// item_index or not and the vect_add_ordered
	// which will use item_index (which will be the
	// place where value should have been) to insert
	// value as an ordered item :)
	p_adaptive_binary_search(v, value, &item_index, f1);

	vect_add_at(v, value, item_index);

DONE_PROCESSING:
#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 2);
#endif
	if(rval)
		p_throw_error(rval, NULL);
}

#endif // ZVECT_DMF_EXTENSIONS

#ifdef ZVECT_SFMD_EXTENSIONS
// Single Function Call Multiple Data operations extensions:

void vect_apply(vector const v, void (*f)(void *)) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif

	zvect_retval rval = p_vect_check(v);
	if (rval)
		goto DONE_PROCESSING;

	// Check parameters:
	if (f == NULL)
		goto DONE_PROCESSING;

	// Process the vector:
	for (register zvect_index i = p_vect_size(v); i--;)
		(*f)(v->data[v->begin + i]);

DONE_PROCESSING:
#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if(rval)
		p_throw_error(rval, NULL);
}

void vect_apply_range(vector const v, void (*f)(void *), const zvect_index x,
                      const zvect_index y) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v, 1);
#endif

	// check if the vector exists:
	zvect_retval rval = p_vect_check(v);
	if (rval)
		goto DONE_PROCESSING;

	// Check parameters:
	if (f == NULL)
		goto DONE_PROCESSING;

	if (x > p_vect_size(v) || y > p_vect_size(v))
	{
		rval = ZVERR_IDXOUTOFBOUND;
		goto DONE_PROCESSING;
	}

	zvect_index start;
	zvect_index end;
	if (x > y) {
		start = x;
		end = y;
	} else {
		start = y;
		end = x;
	}

	// Process the vector:
	for (register zvect_index i = start; i <= end; i++)
		(*f)(v->data[v->begin + i]);

DONE_PROCESSING:
#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v, 1);
#endif
	if(rval)
		p_throw_error(rval, NULL);
}

void vect_apply_if(vector const v1, vector const v2, void (*f1)(void *),
                   bool (*f2)(void *, void *)) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v1, 1);
#endif

	// check if the vector exists:
	zvect_retval rval = p_vect_check(v1) | p_vect_check(v2);
	if (rval)
		goto DONE_PROCESSING;

	// Check parameters:
	if (p_vect_size(v1) > p_vect_size(v2)) {
		rval = ZVERR_VECTTOOSMALL;
		goto DONE_PROCESSING;
	}

	// Process vectors:
	for (register zvect_index i = p_vect_size(v1); i--;)
		if ((*f2)(v1->data[v1->begin + i], v2->data[v2->begin + i]))
			(*f1)(v1->data[v1->begin + i]);

DONE_PROCESSING:
#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v1, 1);
#endif
	if(rval)
		p_throw_error(rval, NULL);
}

void vect_copy(vector const v1, vector const v2, const zvect_index s2,
               const zvect_index e2) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v1, 2);
#endif

	// check if the vectors v1 and v2 exist:
	zvect_retval rval = p_vect_check(v1) | p_vect_check(v2);
	if (rval)
		goto DONE_PROCESSING;

	// We can only copy vectors with the same data_size!
	if (v1->data_size != v2->data_size) {
		rval = ZVERR_VECTDATASIZE;
		goto DONE_PROCESSING;
	}

	// Let's check if the indexes provided are correct for
	// v2:
	if (e2 > p_vect_size(v2) || s2 > p_vect_size(v2)) {
		rval = ZVERR_IDXOUTOFBOUND;
		goto DONE_PROCESSING;
	}

	// If the user specified 0 max_elements then
	// copy the entire vector from start position
	// till the last item in the vector 2:
	zvect_index ee2;
	if (e2 == 0)
		ee2 = (p_vect_size(v2) - 1) - s2;
	else
		ee2 = e2;

	// Set the correct capacity for v1 to get the whole v2:
	while ( p_vect_capacity(v1) <= (p_vect_size(v1) + ee2))
		p_vect_increase_capacity(v1, 1);

	// Copy v2 (from s2) in v1 at the end of v1:
	p_vect_memcpy(v1->data + v1->begin + p_vect_size(v1), v2->data + v2->begin + s2, sizeof(void *) * ee2);

	// Update v1 size:
	v1->end += ee2;

DONE_PROCESSING:
#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v1, 2);
#endif
	if(rval)
		p_throw_error(rval, NULL);
}

/*
 * vect_insert inserts the specified number of elements
 * from vector v2 (from position s2) in vector v1 (from
 * position s1).
 *
 * example: to insert 10 items from v2 (form item at
 *          position 10) into vector v1 starting at
 *          position 5, use:
 * vect_insert(v1, v2, 10, 10, 5);
 */
void vect_insert(vector const v1, vector const v2, const zvect_index s2,
                 const zvect_index e2, const zvect_index s1) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v1, 2);
#endif

	// check if the vectors v1 and v2 exist:
	zvect_retval rval = p_vect_check(v1) | p_vect_check(v2);
	if (rval)
		goto DONE_PROCESSING;

	// We can only copy vectors with the same data_size!
	if (v1->data_size != v2->data_size) {
		rval = ZVERR_VECTDATASIZE;
		goto DONE_PROCESSING;
	}

	// Let's check if the indexes provided are correct for
	// v2:
	if ((e2 > p_vect_size(v2)) || (s2 > p_vect_size(v2))) {
		rval = ZVERR_IDXOUTOFBOUND;
		goto DONE_PROCESSING;
	}

	// If the user specified 0 max_elements then
	// copy the entire vector from start position
	// till the last item in the vector 2:
	zvect_index ee2;
	if (e2 == 0)
		ee2 = (p_vect_size(v2) - 1) - s2;
	else
		ee2 = e2;

	// Process vectors:
	register zvect_index j = 0;

	// Copy v2 items (from s2) in v1 (from s1):
	for (register zvect_index i = s2; i <= s2 + ee2; i++, j++)
		vect_add_at(v1, v2->data[v2->begin + i], s1 + j);

DONE_PROCESSING:
#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v1, 2);
#endif
	if(rval)
		p_throw_error(rval, NULL);
}

/*
 * vect_move moves the specified number of elements
 * from vector v2 (from position s2) in vector v1 (at
 * the end of it).
 *
 * example: to move 10 items from v2 (from item at
 *          position 10) into vector v1, use:
 * vect_move(v1, v2, 10, 10, 5);
 */
void vect_move(vector const v1, vector v2, const zvect_index s2,
               const zvect_index e2) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v1, 2);
#endif

	// check if the vectors v1 and v2 exist:
	zvect_retval rval = p_vect_check(v1) | p_vect_check(v2);
	if (rval)
		goto DONE_PROCESSING;

	// We can only copy vectors with the same data_size!
	if (v1->data_size != v2->data_size) {
		rval = ZVERR_VECTDATASIZE;
		goto DONE_PROCESSING;
	}

	// Let's check if the indexes provided are correct for
	// v2:
	if ((e2 > v2->data_size) || (s2 > p_vect_size(v2))) {
		rval = ZVERR_IDXOUTOFBOUND;
		goto DONE_PROCESSING;
	}

	// If the user specified 0 max_elements then
	// move the entire vector from start position
	// till the last item in the vector 2:
	zvect_index ee2;
	if (e2 == 0)
		ee2 = (p_vect_size(v2) - 1) - s2;
	else
		ee2 = e2;

	// Set the correct capacity for v1 to get the whole v2:
	while (p_vect_capacity(v1) <= (p_vect_size(v1) + ee2))
		p_vect_increase_capacity(v1, 1);

	// Copy v2 (from s2) in v1 at the end of v1:
	p_vect_memcpy(v1->data + v1->begin + p_vect_size(v1), v2->data + v2->begin + s2, sizeof(void *) * ee2);

	// Update v1 size:
	v1->end += ee2;

	for (register zvect_index i = s2; i <= s2 + ee2; i++)
		vect_delete_at(v2, i);

DONE_PROCESSING:
#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v1, 2);
#endif
	if(rval)
		p_throw_error(rval, NULL);
}

void vect_merge(vector const v1, vector v2) {
#if (ZVECT_THREAD_SAFE == 1)
	zvect_retval lock_owner = check_mutex_lock(v1, 2);
#endif

	// check if the vector v1 exists:
	zvect_retval rval = p_vect_check(v1) | p_vect_check(v2);
	if (rval)
		goto DONE_PROCESSING;

	// We can only copy vectors with the same data_size!
	if (v1->data_size != v2->data_size) {
		rval = ZVERR_VECTDATASIZE;
		goto DONE_PROCESSING;
	}

	// Set the correct capacity for v1 to get the whole v2:
	while (p_vect_capacity(v1) <= (p_vect_size(v1) + p_vect_size(v2)))
		p_vect_increase_capacity(v1, 1);

	// Copy the whole v2 in v1 at the end of v1:
	p_vect_memcpy(v1->data + v1->begin + p_vect_size(v1), v2->data + v2->begin, sizeof(void *) * p_vect_size(v2));

	// Update v1 size:
	v1->end += p_vect_size(v2);

DONE_PROCESSING:
#if (ZVECT_THREAD_SAFE == 1)
	if (lock_owner)
		check_mutex_unlock(v1, 2);
#endif

	if(rval)
		p_throw_error(rval, NULL);

	// Because we are merging two vectors in one
	// after merged v2 to v1 there is no need for
	// v2 to still exists, so let's destroy it to
	// free memory correctly:
	p_vect_destroy(v2, 0);
}
#endif

/*---------------------------------------------------------------------------*/
