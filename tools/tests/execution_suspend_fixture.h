/* Host-only stand-ins for production execution ownership boundaries. */
#ifndef RELIEFNT_TEST_EXECUTION_SUSPEND_FIXTURE_H
#define RELIEFNT_TEST_EXECUTION_SUSPEND_FIXTURE_H
#include <stdlib.h>
#include <reliefnt/lock.h>

#ifndef RELIEFNT_TEST_REAL_EXECUTION_GATE
/** @brief Model releasing the host fixture's outer execution transaction.
 * @param token Inactive token. @return Zero with the token active.
 */
__attribute__((weak)) int kernel_execution_suspend_irqrestore(struct kernel_execution_suspend_token *token)
{ if (!token || token->active) return -1; token->active = 1; return 0; }
/** @brief Model reacquiring the host fixture's execution transaction.
 * @param token Active token. @return Zero with the token consumed.
 */
__attribute__((weak)) int kernel_execution_resume_irqsave(struct kernel_execution_suspend_token *token)
{ if (!token || !token->active) return -1; token->active = 0; return 0; }
#endif
/** @brief Fail the fixture if an execution transaction cannot be resumed.
 * @param message Diagnostic from the production code. Does not return.
 */
__attribute__((weak, noreturn)) void panic(const char *message)
{ (void)message; abort(); }
#endif
