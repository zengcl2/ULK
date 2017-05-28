/*
 *  linux/arch/i386/entry.S
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * entry.S contains the system-call and fault low-level handling routines.
 * This also contains the timer-interrupt handler, as well as all interrupts
 * and faults that can result in a task-switch.
 *
 * NOTE: This code handles signal-recognition, which happens every time
 * after a timer-interrupt and after each system call.
 *
 * I changed all the .align's to 4 (16 byte alignment), as that's faster
 * on a 486.
 *
 * Stack layout in 'ret_from_system_call':
 * 	ptrace needs to have all regs on the stack.
 *	if the order here is changed, it needs to be
 *	updated in fork.c:copy_process, signal.c:do_signal,
 *	ptrace.c and ptrace.h
 *
 *	 0(%esp) - %ebx
 *	 4(%esp) - %ecx
 *	 8(%esp) - %edx
 *       C(%esp) - %esi
 *	10(%esp) - %edi
 *	14(%esp) - %ebp
 *	18(%esp) - %eax
 *	1C(%esp) - %ds
 *	20(%esp) - %es
 *	24(%esp) - orig_eax
 *	28(%esp) - %eip
 *	2C(%esp) - %cs
 *	30(%esp) - %eflags
 *	34(%esp) - %oldesp
 *	38(%esp) - %oldss
 *
 * "current" is in register %ebx during any slow entries.
 */

#include <linux/config.h>
#include <linux/linkage.h>
#include <asm/thread_info.h>
#include <asm/errno.h>
#include <asm/segment.h>
#include <asm/smp.h>
#include <asm/page.h>
#include "irq_vectors.h"

#define nr_syscalls ((syscall_table_size)/4)

EBX		= 0x00
ECX		= 0x04
EDX		= 0x08
ESI		= 0x0C
EDI		= 0x10
EBP		= 0x14
EAX		= 0x18
DS		= 0x1C
ES		= 0x20
ORIG_EAX	= 0x24
EIP		= 0x28
CS		= 0x2C
EFLAGS		= 0x30
OLDESP		= 0x34
OLDSS		= 0x38

CF_MASK		= 0x00000001
TF_MASK		= 0x00000100
IF_MASK		= 0x00000200
DF_MASK		= 0x00000400 
NT_MASK		= 0x00004000
VM_MASK		= 0x00020000

#ifdef CONFIG_PREEMPT
#define preempt_stop		cli
#else
#define preempt_stop
#define resume_kernel		restore_all //��û�ж���CONFIG_PREEMPT�������
#endif

/*
 * SAVE_ALL������ջ�б����жϴ���������ܻ�ʹ�õ�����CPU�Ĵ���
 * ��eflags, cs, eip, ss ��esp���⣬��Ϊ��Щ�Ĵ����Ѿ��ɿ��Ƶ�Ԫ�Զ�������
 * Ȼ���������û����ݶε�ѡ���װ��ds��es�Ĵ���
 */
/*
 * �൱�ڰ��жϷ���ʱ,Ӳ��û�б���ļĴ���ѹջ��������.��DS.ES��Ϊ��__USER_DS����һ��ԭ���,�ο��Ͻ�����.
 *
 * ����Ϊ__USER_DS��ԭ��:
 * (���ds��es��fs��gs�μĴ��������ݣ��������һ���Ĵ���������ѡ�����һ������������������DPLֵС��CPL����ô������صĶμĴ�����
 *  ���Ƶ�Ԫ��ô����Ϊ�˽�ֹ�û�̬�ĳ��������ں���ǰ���õĶμĴ���������������Щ�Ĵ����Ļ���������û�����ͻ����������������ں˵�ַ�ռ䡣
 *  ע�⵽4: ����˵��һ��.���ͨ��ϵͳ���ý����ں�̬.Ȼ��DS,ES��ֵ��Ϊ__KERNEL_DS(��2.4 ���ں���),�������(����iret��),
 *           �ָ�CS,EIP��ֵ,��ʱCS��CPL��3.��ΪDS,ES����Ϊ��__KERNEL_DS,������DPL�� 0,����Ҫ��DS,ES�е�ֵ���.��2.6�ں���,�����жϻ��쳣��,
 *           ��DS,ES��ֵ��Ϊ��__USER_DS,�������������������,�����Ч��.)
 */
#define SAVE_ALL \
	cld; \
	pushl %es; \
	pushl %ds; \
	pushl %eax; \
	pushl %ebp; \
	pushl %edi; \
	pushl %esi; \
	pushl %edx; \
	pushl %ecx; \
	pushl %ebx; \
	movl $(__USER_DS), %edx; \
	movl %edx, %ds; \
	movl %edx, %es;

#define RESTORE_INT_REGS \
	popl %ebx;	\
	popl %ecx;	\
	popl %edx;	\
	popl %esi;	\
	popl %edi;	\
	popl %ebp;	\
	popl %eax

#define RESTORE_REGS	\
	RESTORE_INT_REGS; \
1:	popl %ds;	\
2:	popl %es;	\
.section .fixup,"ax";	\
3:	movl $0,(%esp);	\
	jmp 1b;		\
4:	movl $0,(%esp);	\
	jmp 2b;		\
.previous;		\
.section __ex_table,"a";\
	.align 4;	\
	.long 1b,3b;	\
	.long 2b,4b;	\
.previous

#define RESTORE_ALL	\
	RESTORE_REGS	\ //pop��SAVE_ALL����ջ�ļĴ���
	addl $4, %esp;	\  //�Ƿ�?��SAVE_ALL֮ǰѹ����һ���ж������ĸ�ֵ������ϵͳ���ú�
1:	iret;		\ //iret�жϷ���,����Ӳ������жϵķ��ع���
.section .fixup,"ax";   \
2:	sti;		\
	movl $(__USER_DS), %edx; \
	movl %edx, %ds; \
	movl %edx, %es; \
	movl $11,%eax;	\
	call do_exit;	\
.previous;		\
.section __ex_table,"a";\
	.align 4;	\
	.long 1b,2b;	\
.previous


ENTRY(ret_from_fork)
	pushl %eax
	call schedule_tail
	GET_THREAD_INFO(%ebp)
	popl %eax
	jmp syscall_exit

/*
 * Return to user mode is not as complex as all this looks,
 * but we want the default path for a system call return to
 * go as quickly as possible which is why some of this is
 * less clear than it otherwise should be.
 */

	# userspace resumption stub bypassing syscall exit tracing
	ALIGN

/*�쳣���ط���: �쳣���صĺ�벿����IRQ�жϷ������ֻ�Ƕ���һ��preempt_stop�Ĵ���.*/
ret_from_exception:
	/**
	 * ���жϷ���ǰ��handle_IRQ_event����local_irq_disable��ֹ���ж�
	 * �������жϷ���ʱ�����ù��жϣ��������쳣����ʱ����Ҫ���жϹرա�
	 */
	preempt_stop

/*IRQ�жϷ��ط���*/
ret_from_intr:
	/**
	 * �ѵ�ǰthread_info��ص�ebp�С�
	 */
	GET_THREAD_INFO(%ebp) //ȡ�õ�ǰ���̵�task������
	/**
	 * �������ж�EFLAGS��CS��ȷ���Ƿ��������û�̬���Ƿ���VMģʽ��
	 */
	movl EFLAGS(%esp), %eax		# mix EFLAGS and CS //�ж�ǰ��EFLAGS�е�ֵ���EAX
	movb CS(%esp), %al	//���ж�ǰ��CS��16����AL
	//#����EFLAGS ��H16��CS��L16������EAX������
	
	testl $(VM_MASK | 3), %eax //EFLAGS����һλ��ʾ�Ƿ�������vm86ģʽ��,CS����Ͷ�λ��ʾ��ǰ���̵����м���
	/**
	 * ������������ں�̬�����Ҳ���VMģʽ��������resume_kernel��
	 * ������ת��resume_userspace
	 */
	jz resume_kernel		# returning to kernel or vm86-space // ����ж�ǰ�������û��ռ�,�Ҳ�����VM86ģʽ��,��ת��resume_kernel

/**
 * �ָ��û�̬�����������ڡ�
 */
ENTRY(resume_userspace)
	/**
	 * �����Ϊʲô��Ҫ�����ж�
	 */
	//���ж�,�Է��ж϶�ʧ  
	// TODO: cli���ǹ��ж�ô?
 	cli				# make sure we don't miss an interrupt
					# setting need_resched or sigpending
					# between sampling and the iret
	/**
	 * ���thread_info��flag
	 */
	movl TI_flags(%ebp), %ecx //��task->flags��Ա��ֵ���ecx 
	/**
	 * ���������_TIF_WORK_MASK���κ�һλ���ͱ�ʾ�еȴ�����������
	 * ����work_pending������Щ������¼���
	 * �������restore_all�ص��û�̬��
	 */
	andl $_TIF_WORK_MASK, %ecx	# is there any work to be done on
					# int/exception return? //��������û����?
	jne work_pending //��������û����?
	jmp restore_all //�������鶼��������

#ifdef CONFIG_PREEMPT

/**
 * �����쳣�����жϷ���ʱ����Ҫ���ص��ںˣ�����ת���˴���
 */
//��������ں˱����ó���������ռ������µ�resume_kernel
ENTRY(resume_kernel)
	/**
	 * ��֪���˴�Ϊ����Ҫ�ټ�cli,����Ǵ��жϻ����쳣��ת�������ô�Ѿ��ǹ��ж�״̬�ˡ�
	 * Ҳ���ǻ��������ط���������ɡ�
	 */
	cli
	/**
	 * �����ж��ں��Ƿ�������ռ�������סebp�б������thread_info
	 */
	cmpl $0,TI_preempt_count(%ebp)	# non-zero preempt_count ?
	/**
	 * ��ǰ��������ռ���ͼ���ִ���ں˴��롣����restore_all�ָ�ԭ������
	 */
	jnz restore_all //preempt_countΪ��0����ʾ��ʱ���ܷ�����ռ������ת��restore_all
	/**
	 * ������ռ����Ϊ0�����ж��Ƿ��е�������
	 */
need_resched:
	movl TI_flags(%ebp), %ecx	# need_resched set ?
	/**
	 * �ж��Ƿ��е�������
	 */
	testb $_TIF_NEED_RESCHED, %cl //��ʱpreempt_count=0,���need_resched��־��λ���������ȡ���֮����ת��restore_all
	/**
	 * ����Ҫ���ȣ��ͼ���ִ���ں˴��롣����restore_all�ָ�ԭ������
	 */
	jz restore_all
	/**
	 * ��Ȼ�е������󣬵��ǵ�ǰ�ǹ��ж�״̬����Ȼ�����ǲ������ġ�
	 * ��ʱ�ص��û�̬��ʲô�أ�timer�ж϶����ܱ����ˡ�ϵͳ��ô������
	 * ������ڱ��ָ����ں˿���·�����ڽ��ñ���CPU�жϵ�������У���ôҲ��ת��restore_all�ָ�ԭ������
	 * ����������£������л������ƻ��ں����ݽṹ
	 */
	testl $IF_MASK,EFLAGS(%esp)     # interrupts off (exception path) ? //����Ƿ������쳣���򲻻������ռ���ȣ���ʱpreempt_count=0����need_resched��λ
	jz restore_all
	/**
	 * �����Ҫ���̽����л����͵���
	 * preempt_schedule_irq������PREEMPT_ACTIVE��־�����Ѵ��ں�����ʱ����Ϊ-1��Ȼ���жϲ�����schedule��
	 */
	call preempt_schedule_irq 
	jmp need_resched //��ת��need_resched���ж��Ƿ�����Ҫ����������
#endif

/* SYSENTER_RETURN points to after the "sysenter" instruction in
   the vsyscall page.  See vsyscall-sysentry.S, which defines the symbol.  */

	# sysenter call handler stub
/*
 * ����sysenter_entry֮ǰ��:
 * 1. ��׼���еķ�װ���̰�ϵͳ���ú�װ��eax�Ĵ�����������__kernel_vsyscall()����
 * 2. ����__kernel_vsyscall()��ebp��edx��ecx�����ݱ��浽�û�̬��ջ�У����û�ָ̬�뿽����ebp�У�Ȼ��ִ��sysenterָ��
 * 3. CPU���û�̬�л����ں�̬���ں˿�ʼִ��sysenter_entry()����(��SYSENTER_EIP_MSR�Ĵ���ָ��)
 * ������ַ������SYSENTER_EIP_MSR�У�CPU���û�̬�л����ں�̬���ں˿�ʼִ���������
 */
ENTRY(sysenter_entry)
    /*
     * �����ں˶�ջָ�룬��esp0������װ��esp��esp0�ֶ����Ǵ�ŵ�ǰ���̵��ں˶�ջָ��
	 */
	movl TSS_sysenter_esp0(%esp),%esp
sysenter_past_esp:
    /*
     * �򿪱����ж�
	 */
	sti
	/*
	 * ���û����ݶεĶ�ѡ�������ǰ�û�ջָ�롢eflags�Ĵ������û�����εĶ�ѡ���
	 * �Լ���ϵͳ�����˳�ʱҪִ�е�ָ��ĵ�ַ���浽�ں˶�ջ��
	 */
	pushl $(__USER_DS)
	pushl %ebp
	pushfl
	pushl $(__USER_CS)
	pushl $SYSENTER_RETURN

/*
 * Load the potential sixth argument from user stack.
 * Careful about security.
 */
	cmpl $__PAGE_OFFSET-3,%ebp
	jae syscall_fault
	/*
	 * ��ԭ���ɷ�װ���̴��ݵļĴ�����ֵ�ָ���ebp��
	 */
1:	movl (%ebp),%ebp
    /*
     * ͨ��ִ��һϵ��ָ�����ϵͳ���ô�������ͬsystem_call��Ǵ���ʼ��ָ������
	 */
.section __ex_table,"a"
	.align 4
	.long 1b,syscall_fault
.previous

	pushl %eax
	SAVE_ALL
	GET_THREAD_INFO(%ebp)

	testb $(_TIF_SYSCALL_TRACE|_TIF_SYSCALL_AUDIT),TI_flags(%ebp)
	jnz syscall_trace_entry
	cmpl $(nr_syscalls), %eax
	jae syscall_badsys
	call *sys_call_table(,%eax,4)

	/*
	 * ϵͳ���÷������̽����󣬴�eax���ϵͳ���÷������̵ķ����룬
	 * ��������������ں�ջ�б����û�̬eax�Ĵ���ֵ��λ��
	 */
	movl %eax,EAX(%esp)
	/*
	 * ��ֹ�����жϣ������current��thread_info�ṹ�еı�׼
	 */
	cli
	movl TI_flags(%ebp), %ecx
	/*
	 * ������κα�־�����ã���ô�ڷ��ص��û�̬֮ǰ����Ҫ���һЩ��������ת��syscall_exit_work
	 * ���ִ��iret���ָ����ں�̬��ջ��ȥȡ5��������������CPU�л����û�̬����ʼִ��SYSENTER_RETURN��Ǵ��Ĵ���
	 */
	testw $_TIF_ALLWORK_MASK, %cx
	jne syscall_exit_work
/* if something modifies registers it must also disable sysexit */
	/*
	 * �����־�������㣬�ͻ�ܿ췵�ص��û�̬��
	 * ����ǰ�����һ�Զ�ջֵ���ص�edx��ecx�У�edx���SYSENTER_RETURN��־���ĵ�ַ����ecx��õ�ǰ�û�����ջ��ָ��
	 */
	movl EIP(%esp), %edx
	movl OLDESP(%esp), %ecx
	xorl %ebp,%ebp
	/*
	 * �ָ��ж�
	 */
	sti
	/*
	 * ִ��sysexit
	 * sysexit�Ǻ�sysenter��ԵĻ������ָ��:���������ں�̬�����л����û�̬
	 * 1. ��SYSENTER_CS_MSR(�ں˴���Ķ�ѡ���)�Ĵ�����ֵ��16���õ��Ľ�����ص�cs�Ĵ���(���ص����û�̬����Ķ�ѡ���)
	 * 2. ��edx�Ĵ��������ݿ�����eip�Ĵ���
	 * 3. ��SYSENTER_CS_MSR�Ĵ�����ֵ��24���õ���ֵ���ص�SS�Ĵ���(���ص����û�̬���ݶεĶ�ѡ���)
	 * 4. ��ecx�Ĵ��������ݿ�����esp�Ĵ���
	 * 5. CPU���ں�̬�л����û�̬������ʼִ�����ַ�����edx�е�����ָ��
	 */
	sysexit


	# system call handler stub

/*
 * system_call ���Ȱ�ϵͳ���úź�����쳣������������õ�������CPU�Ĵ������浽��Ӧ��ջ��
 * �������ɿ��Ƶ�Ԫ���Զ������eflags��cs��eip��ss��esp�Ĵ�����ʹ�õ���SAVE_ALL�ꡣ
 * �����Ҳ��ds��es��װ���ں����ݶεĶ�ѡ���
 * 
 */
/*ϵͳ�������*/
ENTRY(system_call)
	pushl %eax			# save orig_eax /*save orig_eax(ϵͳ���ú�).�ڷ���ϵͳ���õ�ʱ��,ϵͳ���úŶ��Ǵ����EAX�е�*/
	SAVE_ALL
	/*
	 * �����ebx�д�ŵ�ǰ���̵�thread_info���ݽṹ�ĵ�ַ������ͨ������ں�ջָ���ֵ������ȡ���õ�4KB��8KB�ı�����ɵ�
	 */
	GET_THREAD_INFO(%ebp)
					# system call tracing in operation //ȡ�õ�ǰ���̵�task������
	/*
	 * ���thread_info��flags�ֶε�_TIF_SYSCALL_TRACE��_TIF_SYSCALL_AUDIT��ʶ֮һ�Ƿ�����Ϊ1��
	 * Ҳ���Ǽ���Ƿ���ĳһ���Գ������ڸ���ִ�г����ϵͳ���õĵ��ã�
	 * ����������������ô���ε���do_syscall_trace����: ��
	 * һ�����������ϵͳ���÷�������ִ��֮ǰ��һ�������
	 * �������ֹͣcurrent��������������Խ����ռ�����current����Ϣ
	 */
	/*������̱�TRACE,��ת��syscall_trace_entry,�������û�б�trace*/
	testb $(_TIF_SYSCALL_TRACE|_TIF_SYSCALL_AUDIT),TI_flags(%ebp)
	jnz syscall_trace_entry 
	/*
	 * ���û�̬���̴�������ϵͳ���úŽ�����Ч�Լ�顣
	 * ������ڻ��ߵ���ϵͳ���÷��ɱ��еı�������ϵͳ���ô����������ֹ
	 */	
	cmpl $(nr_syscalls), %eax //��ϵͳ���ñ���ȡ����Ӧ�Ĵ�������
	jae syscall_badsys //ϵͳ���ú���Ч
syscall_call:
    /*
     * ������eax����������ϵͳ���úŶ�Ӧ���ض���������
     * ��Ϊ���ɱ��е�ÿ������ռ4���ֽڣ�������Ȱ�ϵͳ���úų���4���ټ���sys_call_table���ɱ�����ʼ��ַ
     * Ȼ��������ַ��Ԫ��ȡָ��������̵�ָ�룬�ں˾��ҵ���Ҫ���õķ�������
	 */
	call *sys_call_table(,%eax,4) //call��Ӧ�Ĵ�������
    /*
     * ��ϵͳ���÷������̽���ʱ��system_call������eax������ķ���ֵ��
     * �����������ֵ��������������û�̬eax�Ĵ������Ǹ�ջ��Ԫλ����
	 */	
	movl %eax,EAX(%esp)		# store the return value //������ֵѹ�����ǰ��EAX��
/*ϵͳ���÷��ط���*/
syscall_exit:
    /*
     * �رձ����жϲ���鵱ǰ���̵�thread_info�ṹ�еı�־
	 */
	cli				# make sure we don't miss an interrupt
					# setting need_resched or sigpending
					# between sampling and the iret
	movl TI_flags(%ebp), %ecx
	/*
	 * 0xffff���־λ���룬������еı�־λ��û�б����ã���������ת��retore all
	 * ֻҪ��һ����־�����ã���ô��Ҫ�ڷ����û�̬֮ǰ���һЩ����
	 */
	testw $_TIF_ALLWORK_MASK, %cx	# current->work
	jne syscall_exit_work

//restore_all�������RESTRORE_ALL
restore_all:
    /*
     * �ָ��������ں�ջ�еļĴ�����ֵ����ִ��iret���ָ�������¿�ʼ�û�̬��ִ��
	 */
	RESTORE_ALL

	# perform work that needs to be done immediately before resumption
	ALIGN

/**
 * �ڻص��û�̬ǰ������й�������񣬾ʹ�����Щ���������
 */
work_pending:
	/**
	 * ����Ƿ���Ҫ���µ��ȡ�
	 */
	testb $_TIF_NEED_RESCHED, %cl //�ڷ����û��ռ�ʱ��ֻ��Ҫ�ж�need_resched�Ƿ���λ������Ҫ�ж�preempt_count(�û��ռ�)�������λ�������ȣ���֮����ת��work_notifysig
	/**
	 * ����Ҫ���µ��ȣ���Ҫ�ص��û�̬���ڻص��û�̬ǰ���ȼ����������źţ�����work_notifysigƬ��
	 */
	jz work_notifysig
/**
 * �����е�����Ҫ���������ȡ�
 */
work_resched:
	/**
	 * ����һ�¡�����û�е���ȥ��Ҳ���ܳ�ȥ���ֵ��Ȼ����ˡ�
	 */
	call schedule
	/**
	 * ��Ϊ�����ǵ��ȳ�ȥ���ֻ����ˣ�������Ҫ���¹��жϡ�
	 */
	cli				# make sure we don't miss an interrupt
					# setting need_resched or sigpending
					# between sampling and the iret
	/**
	 * ������������Ƿ��е������أ���
	 */
	/*
	 * �ٴν��м�飬���ֻ������TIF_SYSCALL_TRACE, TIF_SYSCALL_AUDIT, �� TIF_SINGLESTEP������˵û�ȴ�Ҫ�������¼��ˣ�����restore_all�ص��û�̬��
	 */
	movl TI_flags(%ebp), %ecx
	andl $_TIF_WORK_MASK, %ecx	# is there any work to be done other
					# than syscall tracing?
	jz restore_all
	/*
	 * �����鵽����Ҫ���ȣ��ͻص�work_resched
	 */
	testb $_TIF_NEED_RESCHED, %cl
	jnz work_resched //���need_resched�ٴα���λ����������ȣ���֮����ִ��work_notifysig

/**
 * ���ˣ����е����˵��û�����µ��ȵ�Ҫ��
 * ����˵�е���Ҫ�󣬵��ǵ��ȳ������ֻ����ˡ�
 * ��֮��������û�е���Ҫ���ˡ��ڻص��û�̬ǰ�������źš�
 * ��Ҫע����ǣ��������в�ֹ��һ���ط����롣
 * һ�Ǵ���������ת�룬���ǴӸ������jmp���롣
 * �������������¼���Ҫ������һ���źţ�����VM86ģʽ��
 * ����VM86ģʽ���ǲ�̫���ġ��ź��أ��ܸ��ӵ����̣�������Ҫһ�²�˵�����Ҳ�Թ���
 */
work_notifysig:				# deal with pending signals and
					# notify-resume requests
	/*
	 * ����û�̬�����eflags�Ĵ�����VM���Ʊ�־�������ˣ��ͽ���work_notifysig_v86
	 */
	testl $VM_MASK, EFLAGS(%esp) #VMģʽ?
	movl %esp, %eax
	jne work_notifysig_v86		# returning to kernel-space or
					# vm86-space
	xorl %edx, %edx
	call do_notify_resume //�����źŴ���
	jmp restore_all

	ALIGN
work_notifysig_v86:
	pushl %ecx			# save ti_flags for do_notify_resume
	/*
	 * ����save_v86_state�������û�̬��ַ�ռ佨������8086ģʽ�����ݽṹ
	 */
	call save_v86_state		# %eax contains pt_regs pointer
	popl %ecx
	movl %eax, %esp
	xorl %edx, %edx
	/*
	 * ����do_notify_resume����������źź͵���ִ��
	 */
	call do_notify_resume
	/*
	 * ��ת��restore_allƬ�Σ��ָ����жϵĳ���
	 */
	jmp restore_all

	# perform syscall exit tracing
	ALIGN
syscall_trace_entry:
	movl $-ENOSYS,EAX(%esp)
	movl %esp, %eax
	xorl %edx,%edx
	call do_syscall_trace
	movl ORIG_EAX(%esp), %eax
	cmpl $(nr_syscalls), %eax
	jnae syscall_call
	jmp syscall_exit

	# perform syscall exit tracing
	ALIGN

syscall_exit_work:
    /*
     * ���_TIF_SYSCALL_TRACE��־�����ã�system_call�����͵ڶ��ε���do_syscall_trace()
     * Ȼ����ת��resume_userspace
     * ���_TIF_SYSCALL_TRACEû�����ã���ת��work_pending
     * ��resume_userspace��work_pending���Ĵ��������µ�����������8086ģʽ�������źź͵���ִ��
     * ������ת��restore all��Ǵ��Իָ��û�̬���̵�ִ��
	 */
	testb $(_TIF_SYSCALL_TRACE|_TIF_SYSCALL_AUDIT|_TIF_SINGLESTEP), %cl
	jz work_pending
	sti				# could let do_syscall_trace() call
					# schedule() instead
	movl %esp, %eax
	movl $1, %edx
	call do_syscall_trace
	jmp resume_userspace

	ALIGN
syscall_fault:
	pushl %eax			# save orig_eax
	SAVE_ALL
	GET_THREAD_INFO(%ebp)
	movl $-EFAULT,EAX(%esp)
	jmp resume_userspace

	ALIGN
syscall_badsys:
    /*
     * ���ϵͳ���ú���Ч���ú����Ͱ�-ENOSYSֵ�����������eax�Ĵ����ĵ�Ԫ��
     * Ȼ������resume_userspace�����������ָ̻������û�̬��ִ��ʱ������eax�з���һ�����ķ�����
	 */
	movl $-ENOSYS,EAX(%esp)
	jmp resume_userspace

/*
 * Build the entry stubs and pointer table with
 * some assembler magic.
 */
.data

/*interrupt���鶨��*/
ENTRY(interrupt)
.text


vector=0
ENTRY(irq_entries_start)
.rept NR_IRQS
	ALIGN
/*
 * ���жϺż�ȥ256�Ľ��������ջ�У��ں��ø�����ʾ���е��жϣ���Ϊ������ʾϵͳ���ã�
 * �������������ʱ�򣬿��Զ����е��жϴ�������ִ����ͬ�Ĵ��룬���ͨ�ô��뿪ʼ�ڱ�ǩcommon_interrupt
 */	
/*
 * �൱��,interrupt[i]ִ�����в���:
 * Pushl $i-256	  //�жϺ�ȡ����ѹջ
 * Jmp common_interrupt		 //��ת��һ�ι����Ĵ�������
 */
1:	pushl $vector-256
	jmp common_interrupt
.data
	.long 1b
.text
vector=vector+1
.endr

	ALIGN

/*IRQ���,�ж����*/
common_interrupt:
	/*common_interrupt ���ȵ��� SAVE ALL����Ĵ���*/
	SAVE_ALL
	
	/*�Ĵ��������ջ���ĵ�ַ����ŵ�eax�Ĵ�������*/
	movl %esp,%eax
	/*
	 * ����do_IRQ()����,
	 * ������Ӧ���жϴ�������
	 */
	call do_IRQ
	/* 
	 * ִ��do_IRQ��retָ���ʱ�򣬿���ת�Ƶ�ret_from_intr()����
	 * ���жϷ���
	 */
	jmp ret_from_intr

/**
 * ����������жϴ�������Ļ�����Դ�������BUILD_INTERRUPT������ġ�
 * ������Ĵ�������ջ��ѹ�������ż�256��ֵ��Ȼ����ø߼�C�����������־��ǵͼ�������������ּ�ǰ׺smp_����
 * �磬CALL_FUNCTION_VECTOR���͵Ĵ��������жϵĵͼ�����������call_function_interrupt��������smp_call_function_interrupt�ĸ߼���������
 */
#define BUILD_INTERRUPT(name, nr)	\
ENTRY(name)				\
	pushl $nr-256;			\
	SAVE_ALL			\
	movl %esp,%eax;			\
	call smp_/**/name;		\
	jmp ret_from_intr;

/* The include is where all of the SMP etc. interrupts come from */
#include "entry_arch.h"

/**
 * ����-0
 * ��һ��������ͼִ��������0������ʱ������
 */
ENTRY(divide_error)
	pushl $0			# no error code
	pushl $do_divide_error
	ALIGN

/*ע�������movl�� movl %1, %2�� ��1�е����ݷ���2�У���Ϊ����cld*/
error_code:
	/**
	 * ����߼�C�������õ��ļĴ�����ջ�У���μ��߼�C������
	 */
	pushl %ds
	pushl %eax
	xorl %eax, %eax //EAX�е�ֵ��Ϊ��
	pushl %ebp
	pushl %edi
	pushl %esi
	pushl %edx
	decl %eax			# eax = -1
	pushl %ecx
	pushl %ebx
	
	/**
	 * cldָ����eflags�����־DF����Ҫ����movs������ָ�
	 */
	cld
	movl %es, %ecx
	/**
	 * ��esp+32���ĸ߼�C������ַ����edi.Ҳ�����쳣���������ĵ�ַ��do_page_fault
	 * ����Ϊʲô��ESP+32����ط���?
	 */
	movl ES(%esp), %edi		# get the function address
	/**
	 * ��esp+36���ĳ����븴�Ƶ�edx�У�����-1д��ջ�С�
	 * ��ע��ǰ��xorl %eax, %eax��decl %eaxһ�䣬����eax����-1
	 * ������������쳣�����λ�þ���-1������ͱ�ʾ��0x80ϵͳ���á�
	 */
	movl ORIG_EAX(%esp), %edx	# get the error code
	movl %eax, ORIG_EAX(%esp)
	movl %ecx, ES(%esp)
	
	/**
	 * ���û����ݶ�ѡ���������ds��es�Ĵ����С�
	 */
	movl $(__USER_DS), %ecx
	movl %ecx, %ds
	movl %ecx, %es
	
	/**
	 * ���ں�ջ�ĵ�ǰջ�����Ƶ�eax������һ���ڴ浥Ԫ�ĵ�ַ��
	 * ���ĵ�ַ�����pushl��ջ�е�ֵ�ĵ�ַ��ͨ�������ַ�Ϳ��Է������б���ļĴ����ˡ�
	 * Ҳ����pt_regs�ṹ��
	 */
	movl %esp,%eax			# pt_regs pointer

	/*
	 * ����Ϊֹ,���Ͼ�Ϊ�жϴ������쳣����������һ��ͳһ�Ķ�ջ,��Ȼ������ֻ��Ϊ��ʹ��ͳһ�Ĳ�������,��struct pt_regs
	 */
	
	/**
	 * ���ø߼�C�����������ú����Ĳ�����ͨ��eax��edx���ݡ�Ҳ���ǵ����쳣��������
	 * ��μ�fastcall void do_page_fault(struct pt_regs *regs, unsigned long error_code)
	 * ���е�fastcall��ʾ�ɼĴ������Σ�regs��ֵ��eax��error_code��ֵ��edx�С�
	 */
	//������Ӧ���쳣��������
	call *%edi
	jmp ret_from_exception

/*16���쳣������ڷ������쳣��������Ҳ�кܶ���ͬ�Ĳ���������ɿ�page_fault*/
ENTRY(coprocessor_error)
	pushl $0 /*��0��ջ.����쳣û�в���һ��Ӳ��������,�Ͱ�0��ջ*/
	pushl $do_coprocessor_error /*��ջ����Ӧ���쳣��������*/
	jmp error_code /*��ת��error_code*/

ENTRY(simd_coprocessor_error)
	pushl $0
	pushl $do_simd_coprocessor_error
	jmp error_code

/**
 * ����-7
 * cr0��TS��־�����ã�ESCAPE��MMX����XMMָ�ִ��
 */
ENTRY(device_not_available)
	pushl $-1			# mark this as an int
	SAVE_ALL
	movl %cr0, %eax
	testl $0x4, %eax		# EM (math emulation bit)
	jne device_not_available_emulate
	preempt_stop
	call math_state_restore
	jmp ret_from_exception
device_not_available_emulate:
	pushl $0			# temporary storage for ORIG_EIP
	call math_emulate
	addl $4, %esp
	jmp ret_from_exception

/*
 * Debug traps and NMI can happen at the one SYSENTER instruction
 * that sets up the real kernel stack. Check here, since we can't
 * allow the wrong stack to be used.
 *
 * "TSS_sysenter_esp0+12" is because the NMI/debug handler will have
 * already pushed 3 words if it hits on the sysenter instruction:
 * eflags, cs and eip.
 *
 * We just load the right stack, and push the three (known) values
 * by hand onto the new stack - while updating the return eip past
 * the instruction that would have done it for sysenter.
 */
#define FIX_STACK(offset, ok, label)		\
	cmpw $__KERNEL_CS,4(%esp);		\
	jne ok;					\
label:						\
	movl TSS_sysenter_esp0+offset(%esp),%esp;	\
	pushfl;					\
	pushl $__KERNEL_CS;			\
	pushl $sysenter_past_esp

/**
 * ��������-1
 * �����ڣ�����eflags��TF��־ʱ����һ��ָ����߲������ĵ�ַ����һ���DEBUG�Ĵ����ķ�Χ֮�ڡ�
 */
ENTRY(debug)
	cmpl $sysenter_entry,(%esp)
	jne debug_stack_correct
	FIX_STACK(12, debug_stack_correct, debug_esp_fix_insn)
debug_stack_correct:
	pushl $-1			# mark this as an int
	SAVE_ALL
	xorl %edx,%edx			# error code 0
	movl %esp,%eax			# pt_regs pointer
	call do_debug
	testl %eax,%eax
	jnz restore_all
	jmp ret_from_exception

/*
 * NMI is doubly nasty. It can happen _while_ we're handling
 * a debug fault, and the debug fault hasn't yet been able to
 * clear up the stack. So we first check whether we got  an
 * NMI on the sysenter entry path, but after that we need to
 * check whether we got an NMI on the debug path where the debug
 * fault happened on the sysenter path.
 */
/**
 * δ��-2
 * Ϊ�������жϱ���(����NMI���ŵ��ж�)
 */
ENTRY(nmi)
	cmpl $sysenter_entry,(%esp)
	je nmi_stack_fixup
	pushl %eax
	movl %esp,%eax
	/* Do not access memory above the end of our stack page,
	 * it might not exist.
	 */
	andl $(THREAD_SIZE-1),%eax
	cmpl $(THREAD_SIZE-20),%eax
	popl %eax
	jae nmi_stack_correct
	cmpl $sysenter_entry,12(%esp)
	je nmi_debug_stack_check
nmi_stack_correct:
	pushl %eax
	SAVE_ALL
	xorl %edx,%edx		# zero error code
	movl %esp,%eax		# pt_regs pointer
	call do_nmi
	RESTORE_ALL

nmi_stack_fixup:
	FIX_STACK(12,nmi_stack_correct, 1)
	jmp nmi_stack_correct
nmi_debug_stack_check:
	cmpw $__KERNEL_CS,16(%esp)
	jne nmi_stack_correct
	cmpl $debug - 1,(%esp)
	jle nmi_stack_correct
	cmpl $debug_esp_fix_insn,(%esp)
	jle nmi_debug_stack_fixup
nmi_debug_stack_fixup:
	FIX_STACK(24,nmi_stack_correct, 1)
	jmp nmi_stack_correct

/**
 * ����-0
 * ��int3�ϵ�ָ��(ͨ����debugger����)����
 */
ENTRY(int3)
	pushl $-1			# mark this as an int
	SAVE_ALL
	xorl %edx,%edx		# zero error code
	movl %esp,%eax		# pt_regs pointer
	call do_int3
	testl %eax,%eax
	jnz restore_all
	jmp ret_from_exception

/**
 * ����-4
 * ��eflags��OF(overflow)��־������ʱ��into(������)ָ�ִ��
 */
ENTRY(overflow)
	pushl $0
	pushl $do_overflow
	jmp error_code

/**
 * ����-5
 * ������Ч��Χ֮��Ĳ�������bound(����ַ�߽�)ָ�ִ��
 */
ENTRY(bounds)
	pushl $0
	pushl $do_bounds
	jmp error_code

/**
 * ����-6
 * CPUִ�е�Ԫ��⵽һ����Ч�Ĳ����롣
 */
ENTRY(invalid_op)
	pushl $0
	pushl $do_invalid_op
	jmp error_code

/**
 * �쳣��ֹ-9
 * ���ⲿ����ѧЭ��������������⣨��������80386��
 */
ENTRY(coprocessor_segment_overrun)
	pushl $0
	pushl $do_coprocessor_segment_overrun
	jmp error_code

/**
 * ����-10
 * CPU��ͼ��һ���������л�����Ч��TSS���̡�
 */
ENTRY(invalid_TSS)
	pushl $do_invalid_TSS
	jmp error_code

/**
 * ����-11
 * ����һ�������ڵ��ڴ�Σ�����������Segment-Present��־����0��
 */
ENTRY(segment_not_present)
	pushl $do_segment_not_present
	jmp error_code

/**
 * ����-12
 * ��ͼ����ջ�ν��޵�ָ�������SS��ʶ�Ķβ����ڴ档
 */
ENTRY(stack_segment)
	pushl $do_stack_segment
	jmp error_code

/**
 * ����-13
 * Υ���˱���ģʽ�µı�������֮һ��
 */
ENTRY(general_protection)
	pushl $do_general_protection
	jmp error_code

/**
 * �������ĵ�ַû�б���ȷ�Ķ��루���磬һ���������ĵ�ַ����4�ı�����
 */
ENTRY(alignment_check)
	pushl $do_alignment_check
	jmp error_code

/**
 * ����-14
 * Ѱַ��ҳ�����ڴ棬��Ӧ��ҳ����Ϊ�ա�����Υ����һ�ַ�ҳ�������ơ�
 */
ENTRY(page_fault)
	pushl $do_page_fault
	jmp error_code

#ifdef CONFIG_X86_MCE
/**
 * �쳣��ֹ-18
 * ���������Ƽ�⵽һ��CPU����������ߴ���
 */
ENTRY(machine_check)
	pushl $0
	pushl machine_check_vector
	jmp error_code
#endif

ENTRY(spurious_interrupt_bug)
	pushl $0
	pushl $do_spurious_interrupt_bug
	jmp error_code

.data
/*��
 * Ϊ�˰�ϵͳ���úź���Ӧ�ķ������̹����������ں�������һ��ϵͳ���÷��ɱ�(dispatch table)��
 * ����������sys_call_table������
 */
ENTRY(sys_call_table)
	.long sys_restart_syscall	/* 0 - old "setup()" system call, used for restarting */
	.long sys_exit
	.long sys_fork
	.long sys_read
	.long sys_write
	.long sys_open		/* 5 */
	.long sys_close
	.long sys_waitpid
	.long sys_creat
	.long sys_link
	.long sys_unlink	/* 10 */
	.long sys_execve
	.long sys_chdir
	.long sys_time
	.long sys_mknod
	.long sys_chmod		/* 15 */
	.long sys_lchown16
	.long sys_ni_syscall	/* old break syscall holder */
	.long sys_stat
	.long sys_lseek
	.long sys_getpid	/* 20 */
	.long sys_mount
	.long sys_oldumount
	.long sys_setuid16
	.long sys_getuid16
	.long sys_stime		/* 25 */
	.long sys_ptrace
	.long sys_alarm
	.long sys_fstat
	.long sys_pause
	.long sys_utime		/* 30 */
	.long sys_ni_syscall	/* old stty syscall holder */
	.long sys_ni_syscall	/* old gtty syscall holder */
	.long sys_access
	.long sys_nice
	.long sys_ni_syscall	/* 35 - old ftime syscall holder */
	.long sys_sync
	.long sys_kill
	.long sys_rename
	.long sys_mkdir
	.long sys_rmdir		/* 40 */
	.long sys_dup
	.long sys_pipe
	.long sys_times
	.long sys_ni_syscall	/* old prof syscall holder */
	.long sys_brk		/* 45 */
	.long sys_setgid16
	.long sys_getgid16
	.long sys_signal
	.long sys_geteuid16
	.long sys_getegid16	/* 50 */
	.long sys_acct
	.long sys_umount	/* recycled never used phys() */
	.long sys_ni_syscall	/* old lock syscall holder */
	.long sys_ioctl
	.long sys_fcntl		/* 55 */
	.long sys_ni_syscall	/* old mpx syscall holder */
	.long sys_setpgid
	.long sys_ni_syscall	/* old ulimit syscall holder */
	.long sys_olduname
	.long sys_umask		/* 60 */
	.long sys_chroot
	.long sys_ustat
	.long sys_dup2
	.long sys_getppid
	.long sys_getpgrp	/* 65 */
	.long sys_setsid
	.long sys_sigaction
	.long sys_sgetmask
	.long sys_ssetmask
	.long sys_setreuid16	/* 70 */
	.long sys_setregid16
	.long sys_sigsuspend
	.long sys_sigpending
	.long sys_sethostname
	.long sys_setrlimit	/* 75 */
	.long sys_old_getrlimit
	.long sys_getrusage
	.long sys_gettimeofday
	.long sys_settimeofday
	.long sys_getgroups16	/* 80 */
	.long sys_setgroups16
	.long old_select
	.long sys_symlink
	.long sys_lstat
	.long sys_readlink	/* 85 */
	.long sys_uselib
	.long sys_swapon
	.long sys_reboot
	.long old_readdir
	.long old_mmap		/* 90 */
	.long sys_munmap
	.long sys_truncate
	.long sys_ftruncate
	.long sys_fchmod
	.long sys_fchown16	/* 95 */
	.long sys_getpriority
	.long sys_setpriority
	.long sys_ni_syscall	/* old profil syscall holder */
	.long sys_statfs
	.long sys_fstatfs	/* 100 */
	.long sys_ioperm
	.long sys_socketcall
	.long sys_syslog
	.long sys_setitimer
	.long sys_getitimer	/* 105 */
	.long sys_newstat
	.long sys_newlstat
	.long sys_newfstat
	.long sys_uname
	.long sys_iopl		/* 110 */
	.long sys_vhangup
	.long sys_ni_syscall	/* old "idle" system call */
	.long sys_vm86old
	.long sys_wait4
	.long sys_swapoff	/* 115 */
	.long sys_sysinfo
	.long sys_ipc
	.long sys_fsync
	.long sys_sigreturn
	.long sys_clone		/* 120 */
	.long sys_setdomainname
	.long sys_newuname
	.long sys_modify_ldt
	.long sys_adjtimex
	.long sys_mprotect	/* 125 */
	.long sys_sigprocmask
	.long sys_ni_syscall	/* old "create_module" */ 
	.long sys_init_module
	.long sys_delete_module
	.long sys_ni_syscall	/* 130:	old "get_kernel_syms" */
	.long sys_quotactl
	.long sys_getpgid
	.long sys_fchdir
	.long sys_bdflush
	.long sys_sysfs		/* 135 */
	.long sys_personality
	.long sys_ni_syscall	/* reserved for afs_syscall */
	.long sys_setfsuid16
	.long sys_setfsgid16
	.long sys_llseek	/* 140 */
	.long sys_getdents
	.long sys_select
	.long sys_flock
	.long sys_msync
	.long sys_readv		/* 145 */
	.long sys_writev
	.long sys_getsid
	.long sys_fdatasync
	.long sys_sysctl
	.long sys_mlock		/* 150 */
	.long sys_munlock
	.long sys_mlockall
	.long sys_munlockall
	.long sys_sched_setparam
	.long sys_sched_getparam   /* 155 */
	.long sys_sched_setscheduler
	.long sys_sched_getscheduler
	.long sys_sched_yield
	.long sys_sched_get_priority_max
	.long sys_sched_get_priority_min  /* 160 */
	.long sys_sched_rr_get_interval
	.long sys_nanosleep
	.long sys_mremap
	.long sys_setresuid16
	.long sys_getresuid16	/* 165 */
	.long sys_vm86
	.long sys_ni_syscall	/* Old sys_query_module */
	.long sys_poll
	.long sys_nfsservctl
	.long sys_setresgid16	/* 170 */
	.long sys_getresgid16
	.long sys_prctl
	.long sys_rt_sigreturn
	.long sys_rt_sigaction
	.long sys_rt_sigprocmask	/* 175 */
	.long sys_rt_sigpending
	.long sys_rt_sigtimedwait
	.long sys_rt_sigqueueinfo
	.long sys_rt_sigsuspend
	.long sys_pread64	/* 180 */
	.long sys_pwrite64
	.long sys_chown16
	.long sys_getcwd
	.long sys_capget
	.long sys_capset	/* 185 */
	.long sys_sigaltstack
	.long sys_sendfile
	.long sys_ni_syscall	/* reserved for streams1 */
	.long sys_ni_syscall	/* reserved for streams2 */
	.long sys_vfork		/* 190 */
	.long sys_getrlimit
	.long sys_mmap2
	.long sys_truncate64
	.long sys_ftruncate64
	.long sys_stat64	/* 195 */
	.long sys_lstat64
	.long sys_fstat64
	.long sys_lchown
	.long sys_getuid
	.long sys_getgid	/* 200 */
	.long sys_geteuid
	.long sys_getegid
	.long sys_setreuid
	.long sys_setregid
	.long sys_getgroups	/* 205 */
	.long sys_setgroups
	.long sys_fchown
	.long sys_setresuid
	.long sys_getresuid
	.long sys_setresgid	/* 210 */
	.long sys_getresgid
	.long sys_chown
	.long sys_setuid
	.long sys_setgid
	.long sys_setfsuid	/* 215 */
	.long sys_setfsgid
	.long sys_pivot_root
	.long sys_mincore
	.long sys_madvise
	.long sys_getdents64	/* 220 */
	.long sys_fcntl64
	.long sys_ni_syscall	/* reserved for TUX */
	.long sys_ni_syscall
	.long sys_gettid
	.long sys_readahead	/* 225 */
	.long sys_setxattr
	.long sys_lsetxattr
	.long sys_fsetxattr
	.long sys_getxattr
	.long sys_lgetxattr	/* 230 */
	.long sys_fgetxattr
	.long sys_listxattr
	.long sys_llistxattr
	.long sys_flistxattr
	.long sys_removexattr	/* 235 */
	.long sys_lremovexattr
	.long sys_fremovexattr
	.long sys_tkill
	.long sys_sendfile64
	.long sys_futex		/* 240 */
	.long sys_sched_setaffinity
	.long sys_sched_getaffinity
	.long sys_set_thread_area
	.long sys_get_thread_area
	.long sys_io_setup	/* 245 */
	.long sys_io_destroy
	.long sys_io_getevents
	.long sys_io_submit
	.long sys_io_cancel
	.long sys_fadvise64	/* 250 */
	.long sys_ni_syscall
	.long sys_exit_group
	.long sys_lookup_dcookie
	.long sys_epoll_create
	.long sys_epoll_ctl	/* 255 */
	.long sys_epoll_wait
 	.long sys_remap_file_pages
 	.long sys_set_tid_address
 	.long sys_timer_create
 	.long sys_timer_settime		/* 260 */
 	.long sys_timer_gettime
 	.long sys_timer_getoverrun
 	.long sys_timer_delete
 	.long sys_clock_settime
 	.long sys_clock_gettime		/* 265 */
 	.long sys_clock_getres
 	.long sys_clock_nanosleep
	.long sys_statfs64
	.long sys_fstatfs64	
	.long sys_tgkill	/* 270 */
	.long sys_utimes
 	.long sys_fadvise64_64
	.long sys_ni_syscall	/* sys_vserver */
	.long sys_mbind
	.long sys_get_mempolicy
	.long sys_set_mempolicy
	.long sys_mq_open
	.long sys_mq_unlink
	.long sys_mq_timedsend
	.long sys_mq_timedreceive	/* 280 */
	.long sys_mq_notify
	.long sys_mq_getsetattr
	.long sys_ni_syscall		/* reserved for kexec */
	.long sys_waitid
	.long sys_ni_syscall		/* 285 */ /* available */
	.long sys_add_key
	.long sys_request_key
	.long sys_keyctl

syscall_table_size=(.-sys_call_table)