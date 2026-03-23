/*
 * Copyright (C) 2009 Ronald Lamprecht
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *  
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */
#include "others/TimerGadget.hh"
#include "world.hh"
#include "timer.hh"

namespace enigma {
    namespace {
        const char *const kSemanticAlarmLeftField = "$mp_alarm_left";
        const char *const kSemanticAlarmIntervalField = "$mp_alarm_interval";
        const char *const kSemanticAlarmRepeatField = "$mp_alarm_repeat";
    }  // namespace

    TimerGadget::TimerGadget() : Other() {
        state = ON;
    }
    
    TimerGadget::~TimerGadget() {
        GameTimer.remove_alarm(this);
    }
    
    std::string TimerGadget::getClass() const {
        return "ot_timer";
    }

    void TimerGadget::MpCaptureSemanticState(MpSemanticState &semantic) const {
        semantic.logical_state = state;
        semantic.flags = MpCaptureFlagsForSnapshot();
        semantic.fields.clear();
        semantic.refs.clear();

        Timer::AlarmSnapshot alarm;
        if (GameTimer.snapshot_alarm(const_cast<TimerGadget *>(this), alarm)) {
            semantic.fields.emplace_back(kSemanticAlarmLeftField, Value(alarm.timeleft));
            semantic.fields.emplace_back(kSemanticAlarmIntervalField, Value(alarm.interval));
            semantic.fields.emplace_back(kSemanticAlarmRepeatField, Value(alarm.repeatp));
        }
    }

    bool TimerGadget::MpApplySemanticState(const MpSemanticState &semantic, MpApplyContext ctx) {
        (void)ctx;
        double alarm_left = 0.0;
        double alarm_interval = 0.0;
        bool alarm_repeat = false;
        bool have_alarm_left = false;
        bool have_alarm_interval = false;
        bool have_alarm_repeat = false;

        for (const auto &field : semantic.fields) {
            if (field.first == kSemanticAlarmLeftField) {
                alarm_left = static_cast<double>(field.second);
                have_alarm_left = true;
            } else if (field.first == kSemanticAlarmIntervalField) {
                alarm_interval = static_cast<double>(field.second);
                have_alarm_interval = true;
            } else if (field.first == kSemanticAlarmRepeatField) {
                alarm_repeat = field.second.to_bool();
                have_alarm_repeat = true;
            }
        }

        MpRestoreFlagsForSnapshot(semantic.flags);
        state = semantic.logical_state;
        GameTimer.remove_all_alarms(this);
        if (have_alarm_left && have_alarm_interval) {
            const bool repeat = have_alarm_repeat ? alarm_repeat : false;
            GameTimer.restore_alarm(this, alarm_interval, alarm_left, repeat);
        }
        return true;
    }

    bool TimerGadget::MpNeedsSemanticWorldResync() const {
        return true;
    }

    int TimerGadget::externalState() const {
        return state == OFF ? 0 : 1;
    }
    
    void TimerGadget::setState(int extState) {
        if (objFlags & OBJBIT_ADDED) {
            if (extState != externalState()) {
                state = extState;
                if (extState == 1) {
                    updateAlarm();
                } else {
                    GameTimer.remove_alarm(this);
                }
            }
        } else {
            state = extState;
        }
    }
    
    void TimerGadget::postAddition() {
        objFlags |= OBJBIT_ADDED;
        updateAlarm();
        Other::postAddition();
    }
    
    void TimerGadget::preRemoval() {
        objFlags &= ~OBJBIT_ADDED;
        Other::preRemoval();
    }
        
    void TimerGadget::alarm() {
        bool actionValue = (state == ON_TRUE);
        state ^= 1;   // toggle between ON_TRUE and ON_FALSE
        if(!getAttr("loop").to_bool()) {
            setState(OFF);
        }
        performAction(actionValue);
    }
    
    void TimerGadget::updateAlarm() {
        if (state == ON) {
            state = ON_TRUE;
            GameTimer.set_alarm(this, (double)getAttr("interval"), getAttr("loop").to_bool());
        }
    }

    BOOT_REGISTER_START
        BootRegister(new TimerGadget(), "ot_timer");
    BOOT_REGISTER_END

} // namespace enigma
