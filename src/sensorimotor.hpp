#ifndef SUPREME_SENSORIMOTOR_HPP
#define SUPREME_SENSORIMOTOR_HPP

#include <cassert>
#include <algorithm>

#include <common/modules.h>
#include "communication_interface.hpp"

namespace supreme {

/*

 +------------------------------+
 | Supreme Sensorimotor Library |
 +------------------------------+-------------+
 | TODO: Description                          |
 |
 |
 |
 +--------------------------------------------+

*/

inline double pos(double value) { return std::max(.0, value); }
inline double neg(double value) { return std::min(.0, value); }


inline double posneg(double value, double p, double n) { return p*pos(value) + n*neg(value); }

inline double uint_to_sc(uint16_t word) { return (word - 512) / 512.0; }

class sensorimotor
{
public:
    /**TODO convert all items to double !! */
    struct interface_data {
        double output_voltage  = 0.0;
        double position        = 0.0;
        double current         = 0.0;
        double voltage_backemf = 0.0;
        double voltage_supply  = 0.0;
        double temperature     = 0.0;
    };

private:
    static const unsigned max_response_time_us = 1000;
    static const unsigned byte_delay_us = 1;
    static const unsigned ping_timeout_us = 50;

    constexpr static const double voltage_scale = 0.012713472; /* Vmax = 13V -> 1023 */
    constexpr static const double current_scale = 0.003225806; /* Imax = 3A3 -> 1023 */

    const uint8_t             motor_id;
    communication_interface&  com;
    bool                      do_request = true;
    bool                      is_responding = false;

    int16_t                   direction = 1;

    interface_data data;

    double                    err_int = 0.0;
    double                    z = 0.0;
    double                    target_position = .0;
    double                    target_voltage  = .0;
    double                    target_csl_mode = .0;
    double                    target_csl_fb   = 1.03;
    double                    limit_lo        = -0.8;
    double                    limit_hi        = +0.8;
    double                    Kp              = 0.8;
    double                    phi_disable     = 0.90;

    enum command_state_t {
        sync0,
        sync1,
        processing,
        completed,
        invalid,
    } syncstate = sync0;

    /* statistics */
    struct Statistics_t {
        unsigned errors = 0;
        unsigned timeouts = 0;
        unsigned response_time_us = 0;
        float    avg_resp_time_us = 0.0;
        unsigned max_resp_time_us = 0;

        bool faulted = false;

        void update(unsigned time_us, bool timeout, bool invalid) {
            if (invalid) ++errors;
            if (timeout) ++timeouts;
            faulted = timeout or invalid;
            response_time_us = time_us;
            avg_resp_time_us = 0.99*avg_resp_time_us + 0.01*time_us;
            max_resp_time_us = std::max(max_resp_time_us, time_us);
        }

    } statistics;

public:

    enum Controller_t {
        none     = 0,
        voltage  = 1,
        position = 2,
        csl      = 3,
    } controller = none;

    sensorimotor(uint8_t id, communication_interface& com)
    : motor_id(id)
    , com(com)
    , data()
    , statistics()
    {}

    /* returns the motors data, such as position, current etc. */
    const interface_data& get_data(void) const { return data; }

    /* returns the motors_id */
    const uint8_t get_id(void) const { return motor_id; }

    /* returns the last known response to ping status */
    const bool is_active(void) const { return is_responding; }

    /* disables the output stage of the motor by sending data requests only */
    void disable(void) { controller = Controller_t::none; }

    bool ping(void) {
        is_responding = false;
        enqueue_command_ping();
        com.read_msg();
        com.send_msg();
        receive_response(ping_timeout_us);
        return is_responding;
    }

    /* performs a full communication cycle */
    Statistics_t execute_cycle(void) {
        if (not do_request) return Statistics_t();

        assert(send_command()); // TODO: handle connection lost
        return receive_response();
    }


    const Statistics_t& get_stats(void) const { return statistics; }
    void reset_statistics(void) { statistics = Statistics_t(); }

    void set_controller_type(Controller_t type) { controller = type; }

    Controller_t get_controller_type(void) const { return controller; }

    void toggle_request(void) { do_request = not do_request; }
    void toggle_led(void) { assert(false); /*TODO implement*/ }


    void set_proportional(double p) { Kp = p; }
    void set_limits(double lo, double hi) { limit_hi = hi; limit_lo = lo; }
    void set_target_csl_mode(double m) { target_csl_mode = m; }
    void set_target_csl_fb  (double f) { target_csl_fb   = f; }
    void set_target_position(double p) { target_position = p; }
    void set_target_voltage (double v) { target_voltage  = v; data.output_voltage = v; }

    void set_direction(int16_t dir) { direction = dir; }

    void set_phi_disable(double phi) { phi_disable = phi; }

    void execute_controller(void) {


        double phi = data.position;

        if (std::abs(phi) >= phi_disable)
            disable();

        if (controller == Controller_t::position)
        {
            const double pos = data.position;
            double err = target_position - pos;

            err_int += err;
            err_int = clip(err_int);

            set_target_voltage(Kp*err);
        } else {
            err_int = .0;
        }


        const double mode = clip(target_csl_mode);
        const double gi = posneg(mode, 2.4, 16.0); //TODO on gi change, reset z to correct value
        const double gf = target_csl_fb * pos(mode);

        if (controller == Controller_t::csl) {
            if (phi > limit_hi) z = std::min(z, gi * phi);
            if (phi < limit_lo) z = std::max(z, gi * phi);

            double u = clip(-gi * phi + z);
            z = gi * phi + gf * u;

            set_target_voltage(0.75*u);
        } else {
            z = gi * phi; /* set initial conditions */
        }
    }

private:

    /** TODO: enqueue sync bytes and checksum could be done by someone else since each package is affected */

    void enqueue_command_toggle_led(void) {
        com.enqueue_sync_bytes(0xFF);
        com.enqueue_byte(0xD0);
        com.enqueue_byte(motor_id);
        com.enqueue_checksum();
    }

    void enqueue_command_data_request() {
        com.enqueue_sync_bytes(0xFF);
        com.enqueue_byte(0xC0);
        com.enqueue_byte(motor_id);
        com.enqueue_checksum();
    }

    void enqueue_command_ping(void) {
        com.enqueue_sync_bytes(0xFF);
        com.enqueue_byte(0xE0);
        com.enqueue_byte(motor_id);
        com.enqueue_checksum();
    }

    void enqueue_command_set_voltage(double voltage) {
        voltage = clip(voltage, 0.5); /** pwms higher than 128 currently ignored**/
        voltage *= direction; // correct direction
        com.enqueue_sync_bytes(0xFF);
        if (voltage >= 0.0) {
            com.enqueue_byte(0xB0);
        } else {
            com.enqueue_byte(0xB1);
        }
        com.enqueue_byte(motor_id);
        uint8_t pwm = static_cast<uint8_t>(round(std::abs(voltage) * 255));
        com.enqueue_byte(pwm);
        com.enqueue_checksum();
    }

    std::size_t send_command(void) {
        if (controller != Controller_t::none)
            enqueue_command_set_voltage(target_voltage);
        else
            enqueue_command_data_request();
        com.read_msg(); // read all whats left
        return com.send_msg();
    }

    Statistics_t receive_response(unsigned timeout_us = max_response_time_us)
    {
        /* wait for data until timeout */
        syncstate = sync0;
        unsigned t_us = 0;
        do {
            while(receive_data());
        } while(++t_us < timeout_us and is_pending() and com.wait_us(byte_delay_us));

        statistics.update(t_us, t_us >= timeout_us, !is_data_valid());
        return statistics;
    }

    /* return code true means continue processing, false: wait for next byte */
    bool receive_data(void) {
        com.read_msg();

        switch(syncstate)
        {
            case sync0:
                if (com.empty()) return false;
                if (com.front() == 0xff) { /* receive and eat first sync byte */
                    syncstate = sync1;
                    com.reset_checksum();
                    com.get_byte();
                }
                else { /* Unexpected first sync byte */
                    com.pop(); /* remove byte and try again */
                }
                return true;

            case sync1:
                if (com.empty()) return false;
                if (com.front() == 0xff) { /* receive and eat second sync byte */
                    syncstate = processing;
                    com.get_byte();
                }
                else { /* Unexpected second sync byte */
                    com.pop();         // remove byte..
                    syncstate = sync0; // and try again
                }
                return true;

            case processing: {
                if (com.empty()) return false;
                uint8_t cmd = com.front();
                switch(cmd)
                {
                case 0x80: /* state data */
                    if (com.size() > 12) { // cmd + id + 2pos + 2cur + 2uba + 2usu +2tmp + chk = 13
                        com.get_byte(); /* eat command byte */
                        uint8_t mid = com.get_byte();
                        if (mid == motor_id) {
                            data.position        = uint_to_sc(com.get_word()) * direction;
                            data.current         = com.get_word() * current_scale;
                            data.voltage_backemf = uint_to_sc(com.get_word());
                            data.voltage_supply  = com.get_word() * voltage_scale;
                            data.temperature     = static_cast<int16_t>(com.get_word()) / 100.0;
                            com.get_byte(); /* eat checksum */
                        }
                        syncstate = (motor_id == mid and com.is_checksum_ok()) ? completed : invalid;
                        return true;
                    }
                    return false;

                case 0xE1: /* ping response */
                    if (com.size() > 2) {
                        com.get_byte(); /* eat command byte */
                        uint8_t mid = com.get_byte();
                        com.get_byte(); /* eat checksum */
                        syncstate = (motor_id == mid and com.is_checksum_ok()) ? completed : invalid;
                        is_responding = (syncstate == completed);
                        return true;
                    }
                    return false;

                default:
                    /* received unknown command byte */
                    syncstate = invalid;
                    return false;
                } /* switch cmd */

                assert(false);
                return false;
                }

            case invalid:
                /* done, but failed */
                return false;

            case completed:
                /* done, message received correctly */
                return false;

            default:
                assert(false);
                return false;

        } /* switch syncstate */
        assert(false);
        return false;
    }



    bool is_pending(void) const { return syncstate != completed and syncstate != invalid; }
    bool is_data_valid(void) const { return syncstate != invalid; }

public:
    command_state_t get_syncstate(void) const { return syncstate; }

};


} /* namespace sensorimotor */

#endif /* SUPREME_SENSORIMOTOR_HPP */
