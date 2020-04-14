该库的协程需要用户自己控制,执行对应函数执行切换,但是实际应用中一般都是会遇到阻塞操作,这时该库做不到自动切换

---

# 大概使用流程
先使用coroutine_open创建协程调度器，然后coroutine_new协程，在协程中使用coroutine_yield中断执行，同一个线程中使用coroutine_resume恢复协程执行

# ucontext_t结构体

```
#include <ucontext.h>
//ucontext_t结构体定义,一个ucontext_t至少包括以下四个成员,可能依据不同系统包括其他不同的成员
typedef struct ucontext_t {
	//为当前context执行结束之后要执行的下一个context，
	//若uc_link为空，执行完当前context之后退出程序
	struct ucontext_t* uc_link;
	//执行当前上下文过程中需要屏蔽的信号列表，即信号掩码
	sigset_t uc_sigmask;
	//为当前context运行的栈信息
	stack_t uc_stack;
	// 保存具体的程序执行上下文，如PC值，堆栈指针以及寄存器值等信息。
	//它的实现依赖于底层，是平台硬件相关的。此实现不透明
	mcontext_t uc_mcontext;
	...
};
```

# ucontext族函数

ucontext族函数主要包括以下四个:
```
#include <ucontext.h>
void makecontext(ucontext_t* ucp, void (*func)(), int argc, ...);
int swapcontext(ucontext_t* olducp, ucontext_t* newucp);
int getcontext(ucontext_t* ucp);
int setcontext(const ucontext_t* ucp);
```

- makecontext:
makecontext修改通过getcontext取得的上下文ucp,**这意味着调用makecontext前必须先调用getcontext**。然后给该上下文指定一个栈空间ucp->stack，设置后继的上下文ucp->uc_link.
当上下文通过setcontext或者swapcontext激活后，执行func函数，argc为func的参数个数，后面是func的参数序列。当func执行返回后，继承的上下文被激活，如果继承上下文为NULL时，线程退出

- swapcontext:
保存当前上下文到oucp结构体中，然后激活upc上下文。

- getcontext:
将当前的执行上下文保存在ucp中，以便后续恢复上下文

- setcontext : 
设置当前的上下文为ucp，**setcontext的上下文ucp应该通过getcontext或者makecontext取得**，如果调用成功则不返回。如果上下文是通过调用getcontext()取得,程序会继续执行这个调用。如果上下文是通过调用makecontext取得,程序会调用makecontext函数的第二个参数指向的函数，如果func函数返回,则恢复makecontext第一个参数指向的上下文第一个参数指向的上下文context_t中指向的uc_link.如果uc_link为NULL,则线程退出。

>注意:setcontext执行成功不返回，getcontext执行成功返回0，若执行失败都返回-1。若uc_link为NULL,执行完新的上下文之后程序结束。

如果执行成功，getcontext返回0，setcontext和swapcontext不返回；如果执行失败，getcontext,setcontext,swapcontext返回-1，并设置对于的errno.

简单说来:
1. getcontext 获取当前上下文
2. setcontext 设置当前上下文, 即切换到指定函数
3. swapcontext 保存当前上下文， 切换至指定上下文 
4. makecontext 创建一个新的上下文。

