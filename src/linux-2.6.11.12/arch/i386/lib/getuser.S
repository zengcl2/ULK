/*
 * __get_user functions.
 *
 * (C) Copyright 1998 Linus Torvalds
 *
 * These functions have a non-standard call interface
 * to make them more efficient, especially as they
 * return an error value in addition to the "real"
 * return value.
 */
#include <asm/thread_info.h>


/*
 * __get_user_X
 *
 * Inputs:	%eax contains the address
 *
 * Outputs:	%eax is error code (0 or -EFAULT)
 *		%edx contains zero-extended value
 *
 * These functions should not modify any other registers,
 * as they get called from within inline assembly.
 */

.text
.align 4
.globl __get_user_1
__get_user_1:
	GET_THREAD_INFO(%edx)
	cmpl TI_addr_limit(%edx),%eax
	jae bad_get_user
1:	movzbl (%eax),%edx
	xorl %eax,%eax
	ret

.align 4
.globl __get_user_2
__get_user_2:
    /*
     * eax�Ĵ�������Ҫ��ȡ�ĵ�һ���ֽڵĵ�ַptr
	 */
	addl $1,%eax
	jc bad_get_user
	/*
	 * ��鲢ȷ��Ҫ��ȡ�������ֽڵĵ�ַС��4GB��С��current���̵�addr_limit.seg�ֶ�
	 */
	GET_THREAD_INFO(%edx)
	cmpl TI_addr_limit(%edx),%eax
	jae bad_get_user
	/*
	 * �����ַ��Ч����ִ��movzwl���Ѷ�ȡ�����ݱ��浽edx�Ĵ������������ֽڣ����Ұ��������ֽ���λ0��Ȼ����eax�����÷�����0����ֹ
	 */
2:	movzwl -1(%eax),%edx
	xorl %eax,%eax
	ret

.align 4
.globl __get_user_4
__get_user_4:
	addl $3,%eax
	jc bad_get_user
	GET_THREAD_INFO(%edx)
	cmpl TI_addr_limit(%edx),%eax
	jae bad_get_user
3:	movl -3(%eax),%edx
	xorl %eax,%eax
	ret

bad_get_user:
    /*
     * �����ַ��Ч��������edx����eax����Ϊ-EFAULT����ֹ
	 */
	xorl %edx,%edx
	movl $-14,%eax
	ret

/*
 * ����ǰ���__get_user_1��__get_user_2��__get_user_4�������쳣���޸�����Ϊbad_get_user��������������������쳣��
 * GNU ������αָ��.section��������Աָ����ִ���ļ����Ĳ��ְ���������Ҫִ�еĴ��롣
 * ��ִ���ļ�����һ������Σ��������ο��������λ���Ϊ��
 * .previousαָ��ǿ�Ƴ���ѽ����ŵĴ�����뵽����һ��.sectionαָ��ʱ����Ľ�
 */
.section __ex_table,"a"
	.long 1b,bad_get_user
	.long 2b,bad_get_user
	.long 3b,bad_get_user
.previous