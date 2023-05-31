.globl KXInvocationCall
KXInvocationCall:

// Save and set up frame pointer
pushq %rbp
movq %rsp, %rbp

// Save r12-r15 so we can use them
pushq %r12
pushq %r13
pushq %r14
pushq %r15

// Move the struct RawArguments into r12 so we can mess with rdi
mov %rdi, %r12

// Save the current stack pointer to r15 before messing with it
mov %rsp, %r15

// Copy stack arguments to the stack

// Put the number of arguments into r10
movq 56(%r12), %r10

// Put the amount of stack space needed into r11 (# args << 3)
movq %r10, %r11
shlq $3, %r11

// Put the stack argument pointer into r13
movq 64(%r12), %r13

// Move the stack down
subq %r11, %rsp

// Align the stack
andq $-0x10, %rsp

// Track the current argument number, start at 0
movq $0, %r14

// Copy loop
stackargs_loop:

// Stop the loop when r14 == r10 (current offset equals stack space needed
cmpq %r14, %r10
je done

// Copy the current argument (r13[r14]) to the current stack slot
movq 0(%r13, %r14, 8), %rdi
movq %rdi, 0(%rsp, %r14, 8)

// Increment the current argument number
inc %r14

// Back to the top of the loop
jmp stackargs_loop

done:

// Copy registers over
movq 8(%r12), %rdi
movq 16(%r12), %rsi
movq 24(%r12), %rdx
movq 32(%r12), %rcx
movq 40(%r12), %r8
movq 48(%r12), %r9

// Call the function pointer
callq *(%r12)

// Copy the result registers into the args struct
movq %rax, 72(%r12)
movq %rdx, 80(%r12)

// Restore the stack pointer
mov %r15, %rsp

// Restore r12-15 for the caller
popq %r15
popq %r14
popq %r13
popq %r12

// Restore the frame pointer and return
leave
ret


.globl KXInvocationForwardStret
KXInvocationForwardStret:


// Set %r10 to indicate that this is a stret call
movq $1, %r10

// Jump to the common handler
jmp _KXInvocationForwardCommon


.globl KXInvocationForward
KXInvocationForward:


// Set %r10 to indicate that this is a normal call
movq $0, %r10

// Jump to the common handler
jmp _KXInvocationForwardCommon


.globl _KXInvocationForwardCommon
_KXInvocationForwardCommon:

// Calculate the location of the stack arguments, which are at rsp + 8
movq %rsp, %r11
addq $8, %r11

// Save and set up frame pointer
pushq %rbp
movq %rsp, %rbp

// Push RawArguments components one by one

// Push isStretCall, which is the value of r10
pushq %r10

// Push a dummy rax and rdx
pushq $0
pushq $0

// Push stackArgs pointer, which is in r11
pushq %r11

// Push a dummy stackArgsCount
pushq $0

// Push argument registers
pushq %r9
pushq %r8
pushq %rcx
pushq %rdx
pushq %rsi
pushq %rdi

// Push a dummy fptr
pushq $0

// Save the pointer to the newly constructed struct to pass it to the C function
movq %rsp, %rdi

// Also save it in r12 so we can get to it afterwards
movq %rdi, %r12

// Align the stack
andq $-0x10, %rsp

// Call into C
callq KXInvocationForwardC

// Copy the return value out of the RawArguments
movq 72(%r12), %rax
movq 80(%r12), %rdx

// Restore the frame pointer and return
leave
ret
