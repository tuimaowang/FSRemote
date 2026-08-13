#include "stream/RemoteVideoBaseline.h"

#include <cassert>

int main()
{
    stream::RemoteVideoBaseline baseline;
    baseline.recordReceived();
    baseline.recordReceived();
    baseline.recordPresented(10, true);
    baseline.recordPresented(20, false);
    baseline.recordDropped();

    const auto first = baseline.snapshot(1000);
    assert(first.framesReceived == 2);
    assert(first.framesPresented == 2);
    assert(first.framesDropped == 1);
    assert(first.uiThreadPresentCalls == 1);
    assert(first.frameAgeP95Ms == 20.0);

    baseline.recordPresented(30, false);
    const auto second = baseline.snapshot(2000);
    assert(second.presentedFps == 1.0);
    assert(second.frameAgeP95Ms == 30.0);
    return 0;
}
