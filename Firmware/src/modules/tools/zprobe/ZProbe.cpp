#include "ZProbe.h"

#include "BaseSolution.h"
#include "ConfigReader.h"
#include "Robot.h"
#include "StepperMotor.h"
#include "main.h"
#include "GCode.h"
#include "Conveyor.h"
#include "SlowTicker.h"
#include "Planner.h"
#include "ZProbeStrategy.h"
#include "StepTicker.h"
#include "Dispatcher.h"
#include "OutputStream.h"

// strategies we know about
#include "ThreePointStrategy.h"
#include "DeltaCalibrationStrategy.h"
#include "DeltaGridStrategy.h"
//#include "CartGridStrategy.h"

#define enable_key "enable"
#define probe_pin_key "probe_pin"
#define debounce_ms_key "debounce_ms"
#define slow_feedrate_key "slow_feedrate"
#define fast_feedrate_key "fast_feedrate"
#define return_feedrate_key "return_feedrate"
#define probe_height_key "probe_height"
#define gamma_max_key "gamma_max"
#define max_z_key "max_z"
#define reverse_z_direction_key "reverse_z"
#define dwell_before_probing_key "dwell_before_probing"
#define leveling_key "leveling"
#define calibration_key "calibration"


#define STEPPER Robot::getInstance()->actuators
#define STEPS_PER_MM(a) (STEPPER[a]->get_steps_per_mm())
#define Z_STEPS_PER_MM STEPS_PER_MM(Z_AXIS)

ZProbe::ZProbe() : Module("zprobe")
{
    probing = false;
    invert_override = false;
}

bool ZProbe::configure(ConfigReader& cr)
{
    ConfigReader::section_map_t m;
    if(!cr.get_section("zprobe", m)) {
        printf("configure-zprobe: no zprobe section found\n");
        return false;
    }

    // if the module is disabled -> do nothing
    if(!cr.get_bool(m, enable_key , false)) {
        return false;
    }

    this->pin.from_string( cr.get_string(m, probe_pin_key, "nc" ))->as_input();
    if(!this->pin.connected()) {
        printf("ERROR: config-zprobe: no pin defined\n");
        return false;
    }

    this->debounce_ms = cr.get_float(m, debounce_ms_key, 0);

    // see if a levellng strategy defined
    std::string leveling = cr.get_string(m, leveling_key, "");

    if(!leveling.empty()) {
        // check with each known strategy and load it if it matches
        if(leveling == "three point") {
            // NOTE this strategy is mutually exclusive with the delta calibration strategy
            leveling_strategy = new ThreePointStrategy(this);

        }else if(leveling == "delta grid") {
            leveling_strategy= new DeltaGridStrategy(this);

        // }else if(leveling == "cartesian grid") {
        //     leveling_strategy = new CartGridStrategy(this);

        } else {
            printf("ERROR: config-zprobe: Unknown leveling stratagy: %s", leveling.c_str());
        }

        if(leveling_strategy != nullptr) {
            if(leveling_strategy->configure(cr)) {
                printf("config-zprobe: loaded %s strategy\n", leveling.c_str());
            } else {
                printf("ERROR: config-zprobe: failed to load %s strategy\n", leveling.c_str());
            }
        }
    }

    // see if a calibration strategy defined
    std::string calibration = cr.get_string(m, calibration_key, "");

    if(!calibration.empty()) {
        // check with each known strategy and load it if it matches
        if(calibration == "delta") {
            calibration_strategy= new DeltaCalibrationStrategy(this);

        } else {
            printf("ERROR: config-zprobe: Unknown calibration stratagy: %s", calibration.c_str());
        }

        if(calibration_strategy != nullptr) {
            if(calibration_strategy->configure(cr)) {
                printf("config-zprobe: loaded %s strategy\n", leveling.c_str());
            } else {
                printf("ERROR: config-zprobe: failed to load %s strategy\n", leveling.c_str());
            }
        }
    }

    this->probe_height  = cr.get_float(m, probe_height_key, 5.0F);
    this->slow_feedrate = cr.get_float(m, slow_feedrate_key, 5); // feedrate in mm/sec
    this->fast_feedrate = cr.get_float(m, fast_feedrate_key, 100); // feedrate in mm/sec
    this->return_feedrate = cr.get_float(m, return_feedrate_key, 0); // feedrate in mm/sec
    this->reverse_z     = cr.get_bool(m, reverse_z_direction_key, false); // Z probe moves in reverse direction
    this->max_z         = cr.get_float(m, max_z_key, 0); // maximum zprobe distance

    this->dwell_before_probing = cr.get_float(m, dwell_before_probing_key, 0); // dwell time in seconds before probing

    // register gcodes and mcodes
    using std::placeholders::_1;
    using std::placeholders::_2;

    // G Code handlers
    THEDISPATCHER->add_handler(Dispatcher::GCODE_HANDLER, 29, std::bind(&ZProbe::handle_gcode, this, _1, _2));
    THEDISPATCHER->add_handler(Dispatcher::GCODE_HANDLER, 30, std::bind(&ZProbe::handle_gcode, this, _1, _2));
    THEDISPATCHER->add_handler(Dispatcher::GCODE_HANDLER, 31, std::bind(&ZProbe::handle_gcode, this, _1, _2));
    THEDISPATCHER->add_handler(Dispatcher::GCODE_HANDLER, 32, std::bind(&ZProbe::handle_gcode, this, _1, _2));
    THEDISPATCHER->add_handler(Dispatcher::GCODE_HANDLER, 38, std::bind(&ZProbe::handle_gcode, this, _1, _2));

    THEDISPATCHER->add_handler(Dispatcher::MCODE_HANDLER, 119, std::bind(&ZProbe::handle_mcode, this, _1, _2));
    THEDISPATCHER->add_handler(Dispatcher::MCODE_HANDLER, 670, std::bind(&ZProbe::handle_mcode, this, _1, _2));
    THEDISPATCHER->add_handler(Dispatcher::MCODE_HANDLER, 500, std::bind(&ZProbe::handle_mcode, this, _1, _2));

    // strategies may handle their own mcodes but we need to register them from the strategy themselves

    // we read the probe in this timer
    SlowTicker::getInstance()->attach(100, std::bind(&ZProbe::read_probe, this));

    return true;
}

void ZProbe::read_probe()
{
    if(!probing || probe_detected) return;

    // we check all axis as it maybe a G38.2 X10 for instance, not just a probe in Z
    if(STEPPER[X_AXIS]->is_moving() || STEPPER[Y_AXIS]->is_moving() || STEPPER[Z_AXIS]->is_moving()) {
        // if it is moving then we check the probe, and debounce it
        if(this->pin.get()) {
            if(debounce < debounce_ms) {
                debounce++;

            } else {// we signal the motors to stop, which will preempt any moves on that axis
                // we do all motors as it may be a delta
                for(auto &a : Robot::getInstance()->actuators) a->stop_moving();
                probe_detected = true;
                debounce = 0;
            }

        } else {
            // The endstop was not hit yet
            debounce = 0;
        }
    }

    return;
}

// single probe in Z with custom feedrate
// returns boolean value indicating if probe was triggered
bool ZProbe::run_probe(float& mm, float feedrate, float max_dist, bool reverse)
{
    if(this->pin.get()) {
        // probe already triggered so abort
        return false;
    }

    float maxz = max_dist < 0 ? this->max_z * 2 : max_dist;

    probing = true;
    probe_detected = false;
    debounce = 0;

    // save current actuator position so we can report how far we moved
    float z_start_pos = Robot::getInstance()->actuators[Z_AXIS]->get_current_position();

    if(dwell_before_probing > .0001F) safe_sleep(dwell_before_probing * 1000);

    // move Z down
    bool dir = (!reverse_z != reverse); // xor
    float delta[3] = {0, 0, 0};
    delta[Z_AXIS] = dir ? -maxz : maxz;
    Robot::getInstance()->delta_move(delta, feedrate, 3);

    // wait until finished
    Conveyor::getInstance()->wait_for_idle();

    // now see how far we moved, get delta in z we moved
    // NOTE this works for deltas as well as all three actuators move the same amount in Z
    mm = z_start_pos - Robot::getInstance()->actuators[2]->get_current_position();

    // set the last probe position to the actuator units moved during this home
    Robot::getInstance()->set_last_probe_position(std::make_tuple(0, 0, mm, probe_detected ? 1 : 0));

    probing = false;

    if(probe_detected) {
        // if the probe stopped the move we need to correct the last_milestone as it did not reach where it thought
        Robot::getInstance()->reset_position_from_current_actuator_position();
    }

    return probe_detected;
}

// do probe then return to start position
bool ZProbe::run_probe_return(float& mm, float feedrate, float max_dist, bool reverse)
{
    float save_z_pos = Robot::getInstance()->get_axis_position(Z_AXIS);

    bool ok = run_probe(mm, feedrate, max_dist, reverse);

    // move probe back to where it was
    float fr;
    if(this->return_feedrate != 0) { // use return_feedrate if set
        fr = this->return_feedrate;
    } else {
        fr = this->slow_feedrate * 2; // nominally twice slow feedrate
        if(fr > this->fast_feedrate) fr = this->fast_feedrate; // unless that is greater than fast feedrate
    }

    // absolute move back to saved starting position
    move_z(save_z_pos, fr, false);

    return ok;
}

bool ZProbe::doProbeAt(float &mm, float x, float y)
{
    // move to xy
    move_xy(x, y, getFastFeedrate());
    return run_probe_return(mm, slow_feedrate);
}

bool ZProbe::handle_gcode(GCode& gcode, OutputStream& os)
{
    if(gcode.get_code() >= 29 && gcode.get_code() <= 32) {

        if(this->pin.get()) {
            os.printf("ZProbe triggered before move, aborting command.\n");
            return true;
        }

        if( gcode.get_code() == 30 ) { // simple Z probe
            // first wait for all moves to finish
            Conveyor::getInstance()->wait_for_idle();

            bool set_z = (gcode.has_arg('Z') && !is_rdelta);
            bool probe_result;
            bool reverse = (gcode.has_arg('R') && gcode.get_arg('R') != 0); // specify to probe in reverse direction
            float rate = gcode.has_arg('F') ? gcode.get_arg('F') / 60 : this->slow_feedrate;
            float mm;

            // if not setting Z then return probe to where it started, otherwise leave it where it is
            probe_result = (set_z ? run_probe(mm, rate, -1, reverse) : run_probe_return(mm, rate, -1, reverse));

            if(probe_result) {
                // the result is in actuator coordinates moved
                os.printf("Z:%1.4f\n", mm);

                if(set_z) {
                    // set current Z to the specified value, shortcut for G92 Znnn
                    THEDISPATCHER->dispatch(os, 'G', 92, 'Z', gcode.get_arg('Z'), 0);
                }

            } else {
                os.printf("ZProbe not triggered\n");
            }

            return true;

        } else {
            if(!gcode.has_arg('P')) {
                // find the first strategy to handle the gcode
                if(leveling_strategy->handleGCode(gcode, os)) {
                    return true;

                } else if(calibration_strategy->handleGCode(gcode, os)) {
                    return true;

                } else {
                    os.printf("No strategy found to handle G%d\n", gcode.get_code());
                    return false;
                }

            } else {
                // P paramater selects which strategy to send the code to
                // 0 being the leveling, 1 being the calibration.
                uint16_t i = gcode.get_arg('P');
                if(i == 0) {
                    if(leveling_strategy->handleGCode(gcode, os)) {
                        return true;
                    }

                } else if(i == 1) {
                    if(calibration_strategy->handleGCode(gcode, os)) {
                        return true;
                    }

                } else {
                    os.printf("Only P0 ad P1 supported\n");
                    return false;
                }

                os.printf("strategy #%d did not handle G%d\n", i, gcode.get_code());
            }

            return false;
        }

    } else if(gcode.get_code() == 38 ) { // G38.2 Straight Probe with error, G38.3 straight probe without error
        // linuxcnc/grbl style probe http://www.linuxcnc.org/docs/2.5/html/gcode/gcode.html#sec:G38-probe
        if(gcode.get_subcode() != 2 && gcode.get_subcode() != 3) {
            os.printf("error:Only G38.2 and G38.3 are supported\n");
            return false;
        }

        if(this->pin.get()) {
            os.printf("error:ZProbe triggered before move, aborting command.\n");
            return true;
        }

        // first wait for all moves to finish
        Conveyor::getInstance()->wait_for_idle();

        if(gcode.has_arg('X')) {
            // probe in the X axis
            probe_XYZ(gcode, os, X_AXIS);

        } else if(gcode.has_arg('Y')) {
            // probe in the Y axis
            probe_XYZ(gcode, os, Y_AXIS);

        } else if(gcode.has_arg('Z')) {
            // probe in the Z axis
            probe_XYZ(gcode, os, Z_AXIS);

        } else {
            os.printf("error:at least one of X Y or Z must be specified\n");
        }

        return true;
    }

    return false;
}

bool ZProbe::handle_mcode(GCode& gcode, OutputStream& os)
{
    // M code processing here
    int c;
    switch (gcode.get_code()) {
        case 119:
            c = this->pin.get();
            os.printf(" Probe: %d", c);
            os.set_append_nl(true);
            break;

        case 670:
            if (gcode.has_arg('S')) this->slow_feedrate = gcode.get_arg('S');
            if (gcode.has_arg('K')) this->fast_feedrate = gcode.get_arg('K');
            if (gcode.has_arg('R')) this->return_feedrate = gcode.get_arg('R');
            if (gcode.has_arg('Z')) this->max_z = gcode.get_arg('Z');
            if (gcode.has_arg('H')) this->probe_height = gcode.get_arg('H');
            if (gcode.has_arg('I')) { // NOTE this is temporary and toggles the invertion status of the pin
                invert_override = (gcode.get_arg('I') != 0);
                pin.set_inverting(pin.is_inverting() != invert_override); // XOR so inverted pin is not inverted and vice versa
            }
            if (gcode.has_arg('D')) this->dwell_before_probing = gcode.get_arg('D');
            break;

        case 500: // save settings
            os.printf(";Probe feedrates Slow/fast(K)/Return (mm/sec) max_z (mm) height (mm) dwell (s):\nM670 S%1.2f K%1.2f R%1.2f Z%1.2f H%1.2f D%1.2f\n",
                      this->slow_feedrate, this->fast_feedrate, this->return_feedrate, this->max_z, this->probe_height, this->dwell_before_probing);
            break;

        default:
            return false;
    }
    return true;
}


// special way to probe in the X or Y or Z direction using planned moves, should work with any kinematics
void ZProbe::probe_XYZ(GCode& gcode, OutputStream& os, int axis)
{
    // enable the probe checking in the timer
    probing = true;
    probe_detected = false;
    Robot::getInstance()->disable_segmentation = true; // we must disable segmentation as this won't work with it enabled (beware on deltas probing in X or Y)

    // get probe feedrate in mm/min and convert to mm/sec if specified
    float rate = (gcode.has_arg('F')) ? gcode.get_arg('F') / 60 : this->slow_feedrate;

    // do a regular move which will stop as soon as the probe is triggered, or the distance is reached
    switch(axis) {
        case X_AXIS: move_x(gcode.get_arg('X'), rate, true); break;
        case Y_AXIS: move_y(gcode.get_arg('Y'), rate, true); break;
        case Z_AXIS: move_z(gcode.get_arg('Z'), rate, true); break;
    }

    // coordinated_move returns when the move is finished

    // disable probe checking
    probing = false;
    Robot::getInstance()->disable_segmentation = false;

    // if the probe stopped the move we need to correct the last_milestone as it did not reach where it thought
    // this also sets last_milestone to the machine coordinates it stopped at
    Robot::getInstance()->reset_position_from_current_actuator_position();
    float pos[3];
    Robot::getInstance()->get_axis_position(pos, 3);

    uint8_t probeok = this->probe_detected ? 1 : 0;

    // print results using the GRBL format
    os.printf("[PRB:%1.3f,%1.3f,%1.3f:%d]\n", pos[X_AXIS], pos[Y_AXIS], pos[Z_AXIS], probeok);
    Robot::getInstance()->set_last_probe_position(std::make_tuple(pos[X_AXIS], pos[Y_AXIS], pos[Z_AXIS], probeok));

    if(probeok == 0 && gcode.get_subcode() == 2) {
        // issue error if probe was not triggered and subcode == 2
        os.printf("ALARM: Probe fail\n");
        broadcast_halt(true);
    }
}

// issue a coordinated move in xy, and return when done
// NOTE must use G53 to force move in machine coordinates and ignore any WCS offsets
void ZProbe::move_xy(float x, float y, float feedrate, bool relative)
{
    Robot::getInstance()->push_state();
    Robot::getInstance()->absolute_mode = !relative;
    Robot::getInstance()->next_command_is_MCS = true; // must use machine coordinates in case G92 or WCS is in effect
    OutputStream nullos;
    THEDISPATCHER->dispatch(nullos, 'G', 0, 'X', x, 'Y', y, 'F', feedrate * 60.0F, 0);
    Conveyor::getInstance()->wait_for_idle();
    Robot::getInstance()->pop_state();
}

void ZProbe::move_x(float x, float feedrate, bool relative)
{
    Robot::getInstance()->push_state();
    Robot::getInstance()->absolute_mode = !relative;
    Robot::getInstance()->next_command_is_MCS = true; // must use machine coordinates in case G92 or WCS is in effect
    OutputStream nullos;
    THEDISPATCHER->dispatch(nullos, 'G', 0, 'X', x, 'F', feedrate * 60.0F, 0);
    Robot::getInstance()->pop_state();
}

void ZProbe::move_y(float y, float feedrate, bool relative)
{
    Robot::getInstance()->push_state();
    Robot::getInstance()->absolute_mode = !relative;
    Robot::getInstance()->next_command_is_MCS = true; // must use machine coordinates in case G92 or WCS is in effect
    OutputStream nullos;
    THEDISPATCHER->dispatch(nullos, 'G', 0, 'Y', y, 'F', feedrate * 60.0F, 0);
    Conveyor::getInstance()->wait_for_idle();
    Robot::getInstance()->pop_state();
}

void ZProbe::move_z(float z, float feedrate, bool relative)
{
    Robot::getInstance()->push_state();
    Robot::getInstance()->absolute_mode = !relative;
    Robot::getInstance()->next_command_is_MCS = true; // must use machine coordinates in case G92 or WCS is in effect
    OutputStream nullos;
    THEDISPATCHER->dispatch(nullos, 'G', 0, 'Z', z, 'F', feedrate * 60.0F, 0);
    Conveyor::getInstance()->wait_for_idle();
    Robot::getInstance()->pop_state();
}

// issue home command
void ZProbe::home()
{
    OutputStream nullos;
    THEDISPATCHER->dispatch(nullos, 'G', 28, THEDISPATCHER->is_grbl_mode() ? 2 : 0, 0);
}
