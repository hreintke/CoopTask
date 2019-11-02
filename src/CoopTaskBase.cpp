/*
CoopTaskBase.cpp - Implementation of cooperative scheduling tasks
Copyright (c) 2019 Dirk O. Kaar. All rights reserved.

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "CoopTaskBase.h"
#ifdef ARDUINO
#include <alloca.h>
#else
#include <chrono>
#endif

#if defined(ESP8266)
#include <Schedule.h>
#include <interrupts.h>
using esp8266::InterruptLock;
#elif !defined(ESP32) && defined(ARDUINO)
class InterruptLock {
public:
    InterruptLock() {
        noInterrupts();
    }
    ~InterruptLock() {
        interrupts();
    }
};
#endif

extern "C" {
    // Integration into global yield() and delay()
#if defined(ESP8266) || defined(ESP32)
    void __yield();

    void yield()
    {
        auto self = CoopTaskBase::self();
        if (self) CoopTaskBase::yield(self);
        else __yield();
    }
#elif defined(ARDUINO)
    void yield()
    {
        auto self = CoopTaskBase::self();
        if (self) CoopTaskBase::yield(self);
    }
#endif
#if defined(ESP8266)
    void __delay(unsigned long ms);

    void delay(unsigned long ms)
    {
        auto self = CoopTaskBase::self();
        if (self) CoopTaskBase::delay(self, ms);
        else __delay(ms);
    }
#endif
}

std::array< std::atomic<CoopTaskBase* >, CoopTaskBase::MAXNUMBERCOOPTASKS> CoopTaskBase::runnableTasks{};
std::atomic<uint32_t> CoopTaskBase::runnableTasksCount(0);

CoopTaskBase* CoopTaskBase::current = nullptr;

#ifndef ARDUINO
namespace
{
    uint32_t millis()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    uint32_t micros()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    void delayMicroseconds(uint32_t us)
    {
        const uint32_t start = micros();
        while (micros() - start < us) {}
    }
}
#elif defined(ESP8266) || defined(ESP32)
namespace
{
    const static uint32_t CYCLES_PER_MS = ESP.getCpuFreqMHz() * 1000;
}
#endif

#if defined(ESP8266)
bool CoopTaskBase::usingBuiltinScheduler = false;

bool CoopTaskBase::rescheduleTask(uint32_t repeat_us)
{
    auto stat = run();
    if (sleeping()) return false;
    switch (stat)
    {
    case -1: // exited.
        return false;
        break;
    case 0: // runnable.
        // rather keep scheduling at wrong delayed interval than drop altogether
        if (repeat_us)
        {
            return !schedule_recurrent_function_us([this]() { return rescheduleTask(0); }, 0);
        }
        break;
    default: // delayed for stat milliseconds or microseconds, check delayIsMs().
        uint32_t next_repeat_us = delayIsMs() ? stat * 1000 : stat;
        if (next_repeat_us > 26000000) next_repeat_us = 26000000;
        if (next_repeat_us == repeat_us) break;
        // rather keep scheduling at wrong interval than drop altogether
        return !schedule_recurrent_function_us([this, next_repeat_us]() { return rescheduleTask(next_repeat_us); }, next_repeat_us, [this]() { return !delayed(); });
        break;
    }
    return true;
}
#endif

bool CoopTaskBase::enrollRunnable()
{
    bool enrolled = false;
    bool inserted = false;
    for (int i = 0; i < CoopTaskBase::MAXNUMBERCOOPTASKS; ++i)
    {
#if !defined(ESP32) && defined(ARDUINO)
        InterruptLock lock;
        auto task = runnableTasks[i].load();
        if (!enrolled && nullptr == task)
        {
            runnableTasks[i].store(this);
            enrolled = true;
            inserted = true;
        }
        else if (this == task)
        {
            if (enrolled)
            {
                runnableTasks[i].store(nullptr);
                inserted = false;
            }
            else
            {
                enrolled = true;
            }
            break;
        }
    }
    if (inserted) runnableTasksCount.store(runnableTasksCount.load() + 1);
#else
        CoopTaskBase* cmpTo = nullptr;
        if (!enrolled && runnableTasks[i].compare_exchange_strong(cmpTo, this))
        {
            enrolled = true;
            inserted = true;
        }
        else if (enrolled)
        {
            cmpTo = this;
            if (runnableTasks[i].compare_exchange_strong(cmpTo, nullptr))
            {
                inserted = false;
                break;
            }
        }
        else if (this == runnableTasks[i].load())
        {
            enrolled = true;
            break;
        }
    }
    if (inserted) ++runnableTasksCount;
#endif
    return enrolled;
}

void CoopTaskBase::delistRunnable()
{
    for (int i = 0; i < CoopTaskBase::MAXNUMBERCOOPTASKS; ++i)
    {
#if !defined(ESP32) && defined(ARDUINO)
        InterruptLock lock;
        if (runnableTasks[i].load() == this)
        {
            runnableTasks[i].store(nullptr);
            runnableTasksCount.store(runnableTasksCount.load() - 1);
            break;
        }
#else
        CoopTaskBase* self = this;
        if (runnableTasks[i].compare_exchange_strong(self, nullptr))
        {
            --runnableTasksCount;
            break;
        }
#endif
    }
}

bool IRAM_ATTR CoopTaskBase::scheduleTask(bool wakeup)
{
    if (!*this || !enrollRunnable()) return false;
#if defined(ESP8266)
    bool reschedule = usingBuiltinScheduler && sleeping();
#endif
    if (wakeup)
    {
        sleep(false);
    }
#if defined(ESP8266)
    return !reschedule || schedule_function([this]() { rescheduleTask(1); });
#else
    return true;
#endif
}

#if defined(_MSC_VER)

CoopTaskBase::~CoopTaskBase()
{
    if (taskFiber) DeleteFiber(taskFiber);
    delistRunnable();
}

LPVOID CoopTaskBase::primaryFiber = nullptr;

void __stdcall CoopTaskBase::taskFiberFunc(void* self)
{
    static_cast<CoopTaskBase*>(self)->func();
    static_cast<CoopTaskBase*>(self)->_exit();
}


int32_t CoopTaskBase::initialize()
{
    if (!cont || init) return -1;
    init = true;
    if (*this)
    {
        if (!primaryFiber) primaryFiber = ConvertThreadToFiber(nullptr);
        if (primaryFiber)
        {
            taskFiber = CreateFiber(taskStackSize, taskFiberFunc, this);
            if (taskFiber) return 0;
        }
    }
    cont = false;
    delistRunnable();
    return -1;
}

int32_t CoopTaskBase::run()
{
    if (!cont) return -1;
    if (sleeps.load()) return 0;
    if (delays.load())
    {
        if (delay_ms)
        {
            auto expired = millis() - delay_start;
            if (expired < delay_duration)
            {
                auto delay_rem = delay_duration - expired;
                return static_cast<int32_t>(delay_rem) < 0 ? DELAY_MAXINT : delay_rem;
            }
        }
        else
        {
            auto expired = micros() - delay_start;
            if (expired < delay_duration)
            {
                auto delay_rem = delay_duration - expired;
                if (delay_rem >= DELAYMICROS_THRESHOLD)
                {
                    return static_cast<int32_t>(delay_rem) < 0 ? DELAY_MAXINT : delay_rem;
                }
                ::delayMicroseconds(delay_rem);
            }
        }
        delays.store(false);
        delay_duration = 0;
    }
    current = this;
    if (!init && initialize() < 0) return -1;
    SwitchToFiber(taskFiber);
    current = nullptr;

    // val = 0: init; -1: exit() task; 1: yield task; 2: sleep task; 3: delay task for delay_duration
    cont = cont && (val > 0);
    sleeps.store(sleeps.load() || (val == 2));
    delays.store(delays.load() || (val > 2));

    if (!cont) {
        DeleteFiber(taskFiber);
        taskFiber = NULL;
        delistRunnable();
        return -1;
    }
    switch (val)
    {
    case 1:
    case 2:
        return 0;
        break;
    case 3:
    default:
        return static_cast<int32_t>(delay_duration) < 0 ? DELAY_MAXINT : delay_duration;
        break;
    }
}

uint32_t CoopTaskBase::getFreeStack()
{
    return taskFiber ? taskStackSize : 0;
}

void CoopTaskBase::doYield(uint32_t val) noexcept
{
    self()->val = val;
    SwitchToFiber(primaryFiber);
}

void CoopTaskBase::_delay(uint32_t ms) noexcept
{
    delay_ms = true;
    delay_start = millis();
    delay_duration = ms;
    // CoopTask::run() defers task for delay_duration milliseconds.
    doYield(3);
}

void CoopTaskBase::_delayMicroseconds(uint32_t us) noexcept
{
    if (us < DELAYMICROS_THRESHOLD) {
        ::delayMicroseconds(us);
        return;
    }
    delay_ms = false;
    delay_start = micros();
    delay_duration = us;
    // CoopTask::run() defers task for delay_duration microseconds.
    doYield(3);
}

void CoopTaskBase::_exit() noexcept
{
    self()->val = -1;
    SwitchToFiber(primaryFiber);
}

void CoopTaskBase::_yield() noexcept
{
    doYield(1);
}

void CoopTaskBase::_sleep() noexcept
{
    doYield(2);
}

void IRAM_ATTR CoopTaskBase::sleep(const bool state) noexcept
{
    sleeps.store(state);
    if (!state)
    {
        delays.store(false);
        delay_duration = 0;
    }
}

#elif defined(ESP32)

CoopTaskBase::~CoopTaskBase()
{
    if (taskHandle) vTaskDelete(taskHandle);
    taskHandle = nullptr;
    delistRunnable();
}

void CoopTaskBase::taskFunc(void* _self)
{
    static_cast<CoopTaskBase*>(_self)->func();
    static_cast<CoopTaskBase*>(_self)->_exit();
}

int32_t CoopTaskBase::initialize()
{
    if (!cont || init) return -1;
    init = true;
    if (*this)
    {
        xTaskCreateUniversal(taskFunc, name().c_str(), taskStackSize, this, tskIDLE_PRIORITY, &taskHandle, CONFIG_ARDUINO_RUNNING_CORE);
        if (taskHandle)
        {
            vTaskSuspend(taskHandle);
            return 0;
        }
    }
    cont = false;
    delistRunnable();
    return -1;
}

int32_t CoopTaskBase::run()
{
    if (!cont) return -1;
    if (sleeps.load()) return 0;
    if (delays.load())
    {
        if (delay_ms)
        {
            if (0 == delay_duration && eSuspended != eTaskGetState(taskHandle))
            {
                // fall through, blocked during FreeRTOS delay or asynchronously ready after is specifically handled below
            }
            else
            {
                auto expired = (ESP.getCycleCount() - delay_start) / CYCLES_PER_MS;
                while (expired)
                {
                    delay_start += CYCLES_PER_MS;
                    delay_duration -= 1;
                    --expired;
                }
                if (expired < delay_duration)
                {
                    auto delay_rem = delay_duration - expired;
                    return static_cast<int32_t>(delay_rem) < 0 ? DELAY_MAXINT : delay_rem;
                }
                sleep(false);
            }
        }
        else
        {
            auto expired = micros() - delay_start;
            if (expired < delay_duration)
            {
                auto delay_rem = delay_duration - expired;
                if (delay_rem >= DELAYMICROS_THRESHOLD)
                {
                    return static_cast<int32_t>(delay_rem) < 0 ? DELAY_MAXINT : delay_rem;
                }
                ::delayMicroseconds(delay_rem);
            }
            sleep(false);
        }
    }

    current = this;

    if (!init)
    {
        if (initialize() < 0)
        {
            current = nullptr;
            return -1;
        }
    }
    bool resume = true;
    for (;;)
    {
        auto taskState = eTaskGetState(taskHandle);
        if (eSuspended == taskState)
        {
            if (!resume)
            {
                vTaskPrioritySet(taskHandle, tskIDLE_PRIORITY);
                break;
            }
            resume = false;
            vTaskPrioritySet(taskHandle, 1);
            vTaskResume(taskHandle);
            continue;
        }
        else if (eReady == taskState)
        {
            if (resume)
            {
                resume = false;
                sleep(false);
                delay_duration = 0;
                vTaskPrioritySet(taskHandle, 1);
                continue;
            }
            vPortYield();
            continue;
        }
        else if (eBlocked == taskState)
        {
            if (resume)
            {
                if (!delays.load())
                {
                    vTaskSuspend(taskHandle);
                    continue;
                }
                break;
            }
            vTaskPrioritySet(taskHandle, tskIDLE_PRIORITY);
            if (!delays.exchange(true))
            {
                delay_ms = true;
                delay_start = ESP.getCycleCount();
                delay_duration = 0;
            }
            break;
        }
        else if (eDeleted == taskState)
        {
            cont = false;
            break;
        }
    }

    current = nullptr;

    if (!cont) {
        vTaskDelete(taskHandle);
        taskHandle = nullptr;
        delistRunnable();
        return -1;
    }
    return static_cast<int32_t>(delay_duration) < 0 ? DELAY_MAXINT : delay_duration;
}

uint32_t CoopTaskBase::getFreeStack()
{
    return taskHandle ? uxTaskGetStackHighWaterMark(taskHandle) : 0;
}

void CoopTaskBase::_delay(uint32_t ms) noexcept
{
    delays.store(true);
    delay_ms = true;
    delay_start = ESP.getCycleCount();
    delay_duration = ms;
    vTaskSuspend(taskHandle);
}

void CoopTaskBase::_delayMicroseconds(uint32_t us) noexcept
{
    if (us < DELAYMICROS_THRESHOLD) {
        ::delayMicroseconds(us);
        return;
    }
    delays.store(true);
    delay_ms = false;
    delay_start = micros();
    delay_duration = us;
    vTaskSuspend(taskHandle);
}

void CoopTaskBase::_sleep() noexcept
{
    sleeps.store(true);
    vTaskSuspend(taskHandle);
}

void CoopTaskBase::_yield() noexcept
{
    delay_duration = 0;
    delays.store(false);
    vTaskSuspend(taskHandle);
}

void CoopTaskBase::_exit() noexcept
{
    cont = false;
    vTaskSuspend(taskHandle);
}

void IRAM_ATTR CoopTaskBase::sleep(const bool state) noexcept
{
    sleeps.store(state);
    if (!state)
    {
        delay_duration = 0;
        delays.store(false);
    }
}

CoopTaskBase* CoopTaskBase::self() noexcept
{
    const auto currentTaskHandle = xTaskGetCurrentTaskHandle();
    auto cur = current;
    if (cur && currentTaskHandle == cur->taskHandle) return cur;
    for (int i = 0; i < CoopTaskBase::MAXNUMBERCOOPTASKS; ++i)
    {
        cur = runnableTasks[i].load();
        if (cur && currentTaskHandle == cur->taskHandle) return cur;
    }
    return nullptr;
}

#else

CoopTaskBase::~CoopTaskBase()
{
    delistRunnable();
}

int32_t CoopTaskBase::initialize()
{
    if (!cont || init) return -1;
    init = true;
    // fill stack with magic values to check overflow, corruption, and high water mark
    for (uint32_t pos = 0; pos <= (taskStackSize + sizeof(STACKCOOKIE)) / sizeof(uint32_t); ++pos)
    {
        reinterpret_cast<uint32_t*>(taskStackTop)[pos] = STACKCOOKIE;
    }
#if defined(ARDUINO) || defined(__GNUC__)
    char* bp = static_cast<char*>(alloca(reinterpret_cast<char*>(&bp) - (taskStackTop + taskStackSize + sizeof(STACKCOOKIE))));
#else
#error Setting stack pointer is not implemented on this target
#endif
    //Serial.printf("CoopTask %s: bp = %p, taskStackTop = %p, taskStackTop + taskStackSize + sizeof(STACKCOOKIE) = %p\n", taskName.c_str(), bp, taskStackTop, taskStackTop + taskStackSize + sizeof(STACKCOOKIE));
    func();
    self()->_exit();
    cont = false;
    delete[] taskStackTop;
    taskStackTop = nullptr;
    delistRunnable();
    return -1;
}

int32_t CoopTaskBase::run()
{
    if (!cont) return -1;
    if (sleeps.load()) return 0;
    if (delays.load())
    {
        if (delay_ms)
        {
#ifdef ESP8266
            uint32_t expired;
            if (usingBuiltinScheduler)
            {
                expired = millis() - delay_start;
            }
            else
            {
                expired = (ESP.getCycleCount() - delay_start) / CYCLES_PER_MS;
                while (expired && delay_duration)
                {
                    delay_start += CYCLES_PER_MS;
                    --delay_duration;
                    --expired;
                }
            }
#else
            auto expired = millis() - delay_start;
#endif
            if (expired < delay_duration)
            {
                auto delay_rem = delay_duration - expired;
                return static_cast<int32_t>(delay_rem) < 0 ? DELAY_MAXINT : delay_rem;
            }
        }
        else
        {
            auto expired = micros() - delay_start;
            if (expired < delay_duration)
            {
                auto delay_rem = delay_duration - expired;
                if (delay_rem >= DELAYMICROS_THRESHOLD)
                {
                    return static_cast<int32_t>(delay_rem) < 0 ? DELAY_MAXINT : delay_rem;
                }
                ::delayMicroseconds(delay_rem);
            }
        }
        delays.store(false);
        delay_duration = 0;
    }
    auto val = setjmp(env);
    // val = 0: init; -1: exit() task; 1: yield task; 2: sleep task; 3: delay task for delay_duration
    if (!val) {
        current = this;
        if (!init) return initialize();
        if (*reinterpret_cast<uint32_t*>(taskStackTop + taskStackSize + sizeof(STACKCOOKIE)) != STACKCOOKIE)
        {
            printf("FATAL ERROR: CoopTask %s stack corrupted\n", name().c_str());
            ::abort();
        }

        longjmp(env_yield, 1);
    }
    else
    {
        current = nullptr;
        if (*reinterpret_cast<uint32_t*>(taskStackTop) != STACKCOOKIE)
        {
            printf("FATAL ERROR: CoopTask %s stack overflow\n", name().c_str());
            ::abort();
        }
        cont = cont && (val > 0);
        sleeps.store(sleeps.load() || (val == 2));
        delays.store(delays.load() || (val > 2));
    }
    if (!cont) {
        delete[] taskStackTop;
        taskStackTop = nullptr;
        delistRunnable();
        return -1;
    }
    switch (val)
    {
    case 1:
    case 2:
        return 0;
        break;
    case 3:
    default:
        return static_cast<int32_t>(delay_duration) < 0 ? DELAY_MAXINT : delay_duration;
        break;
    }
}

uint32_t CoopTaskBase::getFreeStack()
{
    if (!taskStackTop) return 0;
    uint32_t pos;
    for (pos = 1; pos < (taskStackSize + sizeof(STACKCOOKIE)) / sizeof(uint32_t); ++pos)
    {
        if (STACKCOOKIE != reinterpret_cast<uint32_t*>(taskStackTop)[pos])
            break;
    }
    return (pos - 1) * sizeof(uint32_t);
}

void CoopTaskBase::doYield(uint32_t val) noexcept
{
    if (!setjmp(env_yield))
    {
        longjmp(env, val);
    }
}

void CoopTaskBase::_delay(uint32_t ms) noexcept
{
    delay_ms = true;
#ifdef ESP8266
    delay_start = usingBuiltinScheduler ? millis() : ESP.getCycleCount();
#else
    delay_start = millis();
#endif
    delay_duration = ms;
    // CoopTask::run() defers task for delay_duration milliseconds.
    doYield(3);
}

void CoopTaskBase::_delayMicroseconds(uint32_t us) noexcept
{
    if (us < DELAYMICROS_THRESHOLD) {
        ::delayMicroseconds(us);
        return;
    }
    delay_ms = false;
    delay_start = micros();
    delay_duration = us;
    // CoopTask::run() defers task for delay_duration microseconds.
    doYield(3);
}

void CoopTaskBase::_exit() noexcept
{
    longjmp(env, -1);
}

void CoopTaskBase::_yield() noexcept
{
    doYield(1);
}

void CoopTaskBase::_sleep() noexcept
{
    doYield(2);
}

void IRAM_ATTR CoopTaskBase::sleep(const bool state) noexcept
{
    sleeps.store(state);
    if (!state)
    {
        delays.store(false);
        delay_duration = 0;
    }
}

#endif // _MSC_VER

void runCoopTasks(const std::function<void(const CoopTaskBase* const task)>& reaper, const std::function<bool(uint32_t ms)>& onDelay)
{
#ifdef ESP32
    static TaskHandle_t yieldGuardHandle = nullptr;
    if (!yieldGuardHandle)
    {
        xTaskCreateUniversal([](void*)
            {
                for (;;)
                {
                    vPortYield();
                }
            }, "YieldGuard", 0x200, nullptr, 1, &yieldGuardHandle, CONFIG_ARDUINO_RUNNING_CORE);
    }
#endif

    uint32_t taskCount = CoopTaskBase::getRunnableTasksCount();
    uint32_t minDelay = ~0UL;
    for (uint32_t i = 0; taskCount && i < CoopTaskBase::getRunnableTasks().size(); ++i)
    {
        auto task = CoopTaskBase::getRunnableTasks()[i].load();
        if (task)
        {
            --taskCount;
            auto runResult = task->run();
            if (runResult < 0 && reaper)
                reaper(task);
            else if (task->delayed() && static_cast<uint32_t>(runResult) < minDelay)
                minDelay = runResult;
        }
    }

    if (minDelay && (!onDelay || onDelay(minDelay)))
    {
#ifdef ESP32
        vTaskSuspend(yieldGuardHandle);
        vTaskDelay(1);
        vTaskResume(yieldGuardHandle);
#endif	
    }
}