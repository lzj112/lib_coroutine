#include "coroutine.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>

#if __APPLE__ && __MACH__
	#include <sys/ucontext.h>
#else 
	#include <ucontext.h>
#endif 

#define STACK_SIZE (1024*1024)
#define DEFAULT_COROUTINE 16

struct coroutine;

//协程调度器
struct schedule {
	char stack[STACK_SIZE];	 //运行时栈空间,为共享栈,1M
	ucontext_t main;		 //主线程的当前上下文
	int nco;				 //协程数
	int cap;				 //最大容量
    int running;             //正在运行的协程id
    struct coroutine **co;   //一维数组,存放所有协程,容量为cap
};

//协程，保存自身上下文信息，主体函数，和栈信息等
struct coroutine {
	coroutine_func func; 	// 协程所用的函数
	void *ud;  				// 协程参数
	ucontext_t ctx; 		// 当前协程上下文
	struct schedule * sch; 	// 该协程所属的调度器
	ptrdiff_t cap; 	 		// 已经分配的内存大小
	ptrdiff_t size;			// 当前协程运行时栈保存起来后的大小
	int status;				// 协程当前的状态
	char *stack; 			// 当前协程的保存起来的运行时栈
};

//新建一个协程
struct coroutine * 
_co_new(struct schedule *S , coroutine_func func, void *ud) {
	struct coroutine * co = malloc(sizeof(*co));
	co->func = func;
	co->ud = ud;
	co->sch = S;
	co->cap = 0;
	co->size = 0;
	co->status = COROUTINE_READY;
	co->stack = NULL;
	return co;
}

//删除一个协程
void
_co_delete(struct coroutine *co) {
	free(co->stack);
	free(co);
}

//创建一个协程调度器,分配内存,赋初值
struct schedule * 
coroutine_open(void) {
	struct schedule *S = malloc(sizeof(*S));
	S->nco = 0;
	S->cap = DEFAULT_COROUTINE;
	S->running = -1;
	//初始化协程数组,容量为cap
	S->co = malloc(sizeof(struct coroutine *) * S->cap);
	memset(S->co, 0, sizeof(struct coroutine *) * S->cap);
	return S;
}

//关闭一个协程调度器，同时清理掉其负责管理的协程
void 
coroutine_close(struct schedule *S) {
	int i;
	for (i=0;i<S->cap;i++) {
		struct coroutine * co = S->co[i];
		if (co) {
			_co_delete(co);
		}
	}
	free(S->co);
	S->co = NULL;
	free(S);
}

//创建一个协程对象,返回新建协程的ID(为序号协程在调度器数组内的下标)
int 
coroutine_new(struct schedule *S, coroutine_func func, void *ud) {
	struct coroutine *co = _co_new(S, func , ud);
	//当前容量已满,进行2倍扩容
	if (S->nco >= S->cap) {
		int id = S->cap;
		S->co = realloc(S->co, S->cap * 2 * sizeof(struct coroutine *));
		//将新扩容的部分初始化
		memset(S->co + S->cap , 0 , sizeof(struct coroutine *) * S->cap);
		//将新创建的协程放入数组
		S->co[S->cap] = co;
		S->cap *= 2;
		++S->nco;
		return id;
	} else {
		int i;
		for (i=0;i<S->cap;i++) {
			int id = (i+S->nco) % S->cap;
			if (S->co[id] == NULL) {
				S->co[id] = co;
				++S->nco;
				return id;
			}
		}
	}
	assert(0);
	return -1;
}

/*
 * 通过low32和hi32 拼出了struct schedule的指针，这里为什么要用这种方式，而不是直接传struct schedule*呢？
 * 因为makecontext的函数指针的参数是int可变列表，在64位下，一个int没法承载一个指针(int 4字节84位，指针8字节64位)
*/
static void
mainfunc(uint32_t low32, uint32_t hi32) {
	uintptr_t ptr = (uintptr_t)low32 | ((uintptr_t)hi32 << 32);
	struct schedule *S = (struct schedule *)ptr;
	int id = S->running;
	struct coroutine *C = S->co[id];
	C->func(S,C->ud);		//调用协程要处理的业务函数 你自己写的,中间有可能会有不断的yield
	_co_delete(C);			//该协程执行完,删除
	S->co[id] = NULL;
	--S->nco;
	S->running = -1;
}

//切换到对应协程中,让调度器运行协程号=id的协程
void 
coroutine_resume(struct schedule * S, int id) {
	assert(S->running == -1);
	assert(id >=0 && id < S->cap);
	//取出协程
	struct coroutine *C = S->co[id];
	if (C == NULL)
		return;
	int status = C->status;
	switch(status) {
		//如果状态是ready也就是第一次创建,由于刚建立 栈信息还没有配置好  所以利用getcontext以及makecontext为其配置上下文
	case COROUTINE_READY:
		//获取当前上下文,初始化ucontext_t结构体,将当前的上下文放到C->ctx里面
		getcontext(&C->ctx);
		//将当前协程的运行时栈的栈顶设置为S->stack,每个协程都这么设置,这就是所谓的共享栈.(注意,这里是栈顶)
		C->ctx.uc_stack.ss_sp = S->stack;
		C->ctx.uc_stack.ss_size = STACK_SIZE;
		//如果协程执行完，将切换到主协程中执行
		C->ctx.uc_link = &S->main;
		//设置当前执行的协程id
		S->running = id;
		C->status = COROUTINE_RUNNING;
		uintptr_t ptr = (uintptr_t)S;
		// 设置执行C->ctx函数, 并将S作为参数传进去
		makecontext(&C->ctx, (void (*)(void)) mainfunc, 2, (uint32_t)ptr, (uint32_t)(ptr>>32));
		// 将当前的上下文放入S->main中，并将C->ctx的上下文替换到当前上下文
		swapcontext(&S->main, &C->ctx);
		//也就是makecontext创建了一个新的上下文,要执行mainfunc了,而保存当前的上下文到S->main,用于执行完切换回来
		break;

	//还有一种就是之前运行过了，然后被调度器调走了，运行别的协程了，
	// 再次回来运行此协程时 状态变为SUSPEND 这个时候因为刚建立时栈信息已经配置好了 所以这里不需要makecontext配置栈信息了 
	// 只需要swapcontext切换到此协程就ok
	case COROUTINE_SUSPEND:
		// 将协程所保存的栈的内容，拷贝到当前运行时栈中
		// 其中C->size在yield时有保存
		// memcpy还是从低地址向高地址增长的,所以dst参数是要转换的协程栈顶
		memcpy(S->stack + STACK_SIZE - C->size, C->stack, C->size);
		S->running = id;
		C->status = COROUTINE_RUNNING;
		swapcontext(&S->main, &C->ctx);
		break;
	default:
		assert(0);
	}
}

//保存当前协程的栈内容
static void
_save_stack(struct coroutine *C, char *top) {
	// linux的内存分布，栈是从高地址向低地址扩展，因此
	// S->stack + STACK_SIZE就是运行时栈的栈底
	// dummy，此时在栈中，并且是最新压入栈的变量，肯定是位于栈顶
	// top - &dummy 即整个栈的容量，top即共享栈的栈底(高地址)
	char dummy = 0;
	assert(top - &dummy <= STACK_SIZE);//dummy地址减栈底地址为当前使用的栈大小
	if (C->cap < top - &dummy) {//如果当前协程栈大小小于已用大小，重新分配
		free(C->stack);
		C->cap = top-&dummy;
		C->stack = malloc(C->cap);
	}
	C->size = top - &dummy;
	//dst是协程栈栈顶,source是dummy的地址，即共享栈的栈顶
	memcpy(C->stack, &dummy, C->size); //将共享栈拷贝到协程栈
}

//将当前正在运行的协程让出，切换到主协程上
void
coroutine_yield(struct schedule * S) {
	// 取出当前正在运行的协程
	int id = S->running;
	assert(id >= 0);
	struct coroutine * C = S->co[id];
	assert((char *)&C > S->stack);
	// 将当前运行的协程的栈内容保存起来
	//存到协程结构体里面的char *stack那个指针里面
	_save_stack(C,S->stack + STACK_SIZE);
	// 将当前栈的状态改为 挂起
	C->status = COROUTINE_SUSPEND;
	S->running = -1;
	//切换回主协程的上下文
	swapcontext(&C->ctx , &S->main);
}

int 
coroutine_status(struct schedule * S, int id) {
	assert(id>=0 && id < S->cap);
	if (S->co[id] == NULL) {
		return COROUTINE_DEAD;
	}
	return S->co[id]->status;
}

int 
coroutine_running(struct schedule * S) {
	return S->running;
}

