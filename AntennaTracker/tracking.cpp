/// -*- tab-width: 4; Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-

#include "Tracker.h"

/**
  update_vehicle_position_estimate - updates estimate of vehicle positions
  should be called at 50hz
 */
void Tracker::update_vehicle_pos_estimate()
{
    // calculate time since last actual position update
    float dt = (hal.scheduler->micros() - vehicle.last_update_us) * 1.0e-6f;

    // if less than 5 seconds since last position update estimate the position
    if (dt < TRACKING_TIMEOUT_SEC) {
        // project the vehicle position to take account of lost radio packets
        vehicle.location_estimate = vehicle.location;
        location_update(vehicle.location_estimate, vehicle.heading, vehicle.ground_speed * dt);
        // set valid_location flag
        vehicle.location_valid = true;
    } else {
        // vehicle has been lost, set lost flag
        vehicle.location_valid = false;
    }
}

/**
  update_tracker_position - updates antenna tracker position from GPS location
  should be called at 50hz
 */
void Tracker::update_tracker_position()
{
    // update our position if we have at least a 2D fix
    // REVISIT: what if we lose lock during a mission and the antenna is moving?
    if (gps.status() >= AP_GPS::GPS_OK_FIX_2D) {
        current_loc = gps.location();

        if (old_loc.alt == 0 && old_loc.lat == 0 && old_loc.lng == 0) {
            old_loc = gps.location();
        }
        current_loc.alt = 0.95f * old_loc.alt + 0.05f * current_loc.alt;
        current_loc.lat = 0.95f * old_loc.lat + 0.05f * current_loc.lat;
        current_loc.lng = 0.95f * old_loc.lng + 0.05f * current_loc.lng;
        old_loc = current_loc;
    }
}

/**
  update_bearing_and_distance - updates bearing and distance to the vehicle
  should be called at 50hz
 */
void Tracker::update_bearing_and_distance()
{
    // exit immediately if we do not have a valid vehicle position or servo_test is in progress
    if (!vehicle.location_valid || control_mode == SERVO_TEST) {
        return;
    }

    // calculate bearing to vehicle
    // To-Do: remove need for check of control_mode
    if (control_mode != SCAN && !nav_status.manual_control_yaw) {
        nav_status.bearing  = get_bearing_cd(current_loc, vehicle.location_estimate) * 0.01f;
    }

    // calculate distance to vehicle
    nav_status.distance = get_distance(current_loc, vehicle.location_estimate);

    // calculate pitch to vehicle
    // To-Do: remove need for check of control_mode
    if (control_mode != SCAN && !nav_status.manual_control_pitch) {
        nav_status.pitch    = degrees(atan2f(nav_status.altitude_difference, nav_status.distance));
    }
}

/**
  main antenna tracking code, called at 50Hz
 */
void Tracker::update_tracking(void)
{
    // update vehicle position estimate
    update_vehicle_pos_estimate();

    // update antenna tracker position from GPS
    update_tracker_position();

    // update bearing and distance to vehicle
    update_bearing_and_distance();

    // do not perform any servo updates until startup delay has passed
    if (g.startup_delay > 0 &&
        hal.scheduler->millis() - start_time_ms < g.startup_delay*1000) {
        return;
    }

    // do not perform updates if safety switch is disarmed (i.e. servos can't be moved)
    if (hal.util->safety_switch_state() == AP_HAL::Util::SAFETY_DISARMED) {
        return;
    }

    switch (control_mode) {
    case AUTO:
        update_auto();
        break;

    case MANUAL:
        update_manual();
		//update_manual_angle(); // Angle-oriented control rather than PWM oriented control
        break;

    case SCAN:
        update_scan();
        break;

    case SERVO_TEST:
        update_servo_test();
        break;
    case STOP:
        disarm_servos();
        break;
    case INITIALISING:
        update_initialising();
        break;
    }
}

/**
   handle an updated position from the aircraft
 */
void Tracker::tracking_update_position(const mavlink_global_position_int_t &msg)
{
    vehicle.location.lat = msg.lat;
    vehicle.location.lng = msg.lon;
    vehicle.location.alt = msg.alt/10;
    vehicle.heading      = msg.hdg * 0.01f;
    vehicle.ground_speed = pythagorous2(msg.vx, msg.vy) * 0.01f;
    vehicle.last_update_us = hal.scheduler->micros();    
    vehicle.last_update_ms = hal.scheduler->millis();
    if (g.alt_source == 1){
        tracking_update_gps_alt();
    }
}


/**
   handle an updated pressure reading from the aircraft
 */
void Tracker::tracking_update_pressure(const mavlink_scaled_pressure_t &msg)
{
    // exit if we use gps altitude
    if (g.alt_source != 0){
        return;
    }
    float local_pressure = barometer.get_pressure();
    float aircraft_pressure = msg.press_abs*100.0f;

    // calculate altitude difference based on difference in barometric pressure
    float alt_diff = barometer.get_altitude_difference(local_pressure, aircraft_pressure);
    if (!isnan(alt_diff)) {
        nav_status.altitude_difference = alt_diff + nav_status.altitude_offset;
    }

    if (nav_status.need_altitude_calibration) {
        // we have done a baro calibration - zero the altitude
        // difference to the aircraft
        nav_status.altitude_offset = -nav_status.altitude_difference;
        nav_status.altitude_difference = 0;
        nav_status.need_altitude_calibration = false;
    }
}

void Tracker::tracking_update_gps_alt()
{
    if (vehicle.location_valid == true){
        nav_status.altitude_difference = (vehicle.location.alt - current_loc.alt) / 100;
    }
}

/**
   handle a manual control message by using the data to command yaw and pitch
 */
void Tracker::tracking_manual_control(const mavlink_manual_control_t &msg)
{
    nav_status.bearing = msg.x;
    nav_status.pitch   = msg.y;
    nav_status.distance = 0.0;
    nav_status.manual_control_yaw   = (msg.x != 0x7FFF);
    nav_status.manual_control_pitch = (msg.y != 0x7FFF);
    // z, r and buttons are not used
}

/**
   update_armed_disarmed - set armed LED if we have received a position update within the last 5 seconds
 */
void Tracker::update_armed_disarmed()
{
    if (vehicle.last_update_ms != 0 && (hal.scheduler->millis() - vehicle.last_update_ms) < TRACKING_TIMEOUT_MS) {
        AP_Notify::flags.armed = true;
    } else {
        AP_Notify::flags.armed = false;
    }
}

int delay_timer=0;  // fixme: this needs to be done properly

void Tracker::update_initialising(void)
{
    // fixed angle for pitch initialising. Zero servo output to prevent erratic movement.
    nav_status.pitch = 45;
       
    channel_yaw.disable_out();
    channel_pitch.enable_out();
    
    float pitch = constrain_float(nav_status.pitch+g.pitch_trim, -90, 90);
    
    update_pitch_servo(pitch);
    
    float ahrs_pitch = degrees(ahrs.pitch);
    if (ahrs_pitch < 50 && ahrs_pitch > 40)
    {
        if (delay_timer == 0) delay_timer = hal.scheduler->millis();
        // PITCH INITIALISING complete, switch to auto mode after delay
        if (g.startup_delay > 0 &&
            hal.scheduler->millis() - delay_timer < g.startup_delay*1000) {
            return;
        }
        set_mode(AUTO);
    }
}
