# IO Monad `poll_if` Keep-Alive Changes

## Overview
The fix introduces explicit keep-alive captures inside the `monad::IO<T>::poll_if` helpers so that the retry lambda survives the asynchronous wait scheduled on a `boost::asio::steady_timer`. Without the additional ownership, the lambda could be destroyed before the timer callback executed, causing polling loops to terminate after the first pending response.

## Symptoms Observed
- Device login polling stopped after the first `authorization_pending` response.
- Subsequent HTTP polls were never issued, and the workflow hung until the overall timeout fired.
- Targeted regression test `LoginHandlerWorkflowTest.PollRetriesBeforeApproval` failed because the handler never received the approval response.

## Changes Made
- Added `keep_alive` shared-pointer captures in both `boost::asio::io_context` and `boost::asio::any_io_executor` overloads of `poll_if` for `IO<T>`.
- Mirrored the same ownership fix for the error retry path to avoid premature destruction when retrying after transport failures.
- Created `include/io_monad_poll_if_legacy.hpp`, a verbatim snapshot of the header prior to the October 2025 changes, for quick reference and potential rollback comparisons.
- Extended the login handler workflow test to cover multiple poll attempts and confirm the loop now progresses until approval.

## Validation
- Rebuilt and executed `./tests/test_login_handler_workflow --gtest_filter=LoginHandlerWorkflowTest.PollRetriesBeforeApproval` (passes in ~3s).

## Next Steps
- Consider porting the keep-alive pattern to other timer-driven combinators if similar lifetime issues arise.
- Remove the legacy header once the change is broadly validated and no longer needed for comparison.
