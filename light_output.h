#pragma once 

#include "light_output.h"

// What seems to be a bug in ESPHome transitioning: when turning on
// the device, the brightness is scaled along with the state (which
// runs from 0 to 1), but when turning off the device, the brightness
// is kept the same while the state goes down from 1 to 0. As a result
// when turning off the lamp with a transition time of 1s, the light
// stays on for 1s and then turn itself off abruptly.
//
// Reported the issue + fix at:
// https://github.com/esphome/esphome/pull/1643
//
// A work-around for this issue can be enabled using the following
// define. Note that the code provides a forward-compatible fix, so
// having this define active with a fixed ESPHome version should
// not be a problem.
#define TRANSITION_TO_OFF_BUGFIX

namespace esphome {
namespace yeelight {
namespace bs2 {

    static const char *TAG = "yeelight_bs2";

    // Same range as supported by the original Yeelight firmware.
    static const int HOME_ASSISTANT_MIRED_MIN = 153;
    static const int HOME_ASSISTANT_MIRED_MAX = 588;

    class LightStateDataExposer {
    public:
        virtual bool has_active_transformer() = 0;
        virtual light::LightColorValues get_transformer_values() = 0;
        virtual light::LightColorValues get_transformer_end_values() = 0;
        virtual float get_transformer_progress() = 0;
    };

    class YeelightBS2LightOutput : public Component, public light::LightOutput {
    public:
        light::LightTraits get_traits() override
        {
            auto traits = light::LightTraits();
            traits.set_supports_rgb(true);
            traits.set_supports_color_temperature(true);
            traits.set_supports_brightness(true);
            traits.set_supports_rgb_white_value(false);
            traits.set_supports_color_interlock(true);
            traits.set_min_mireds(HOME_ASSISTANT_MIRED_MIN);
            traits.set_max_mireds(HOME_ASSISTANT_MIRED_MAX);
            return traits;
        }

        void set_red_output(ledc::LEDCOutput *red) {
            red_ = red;
        }

        void set_green_output(ledc::LEDCOutput *green) {
            green_ = green;
        }

        void set_blue_output(ledc::LEDCOutput *blue) {
            blue_ = blue;
        }

        void set_white_output(ledc::LEDCOutput *white) {
            white_ = white;
        }

        void set_master1_output(gpio::GPIOBinaryOutput *master1) {
            master1_ = master1;
        }

        void set_master2_output(gpio::GPIOBinaryOutput *master2) {
            master2_ = master2;
        }

        void set_light_state_data_exposer(LightStateDataExposer *exposer) {
            state_exposer_ = exposer;
        }

        void write_state(light::LightState *state)
        {
            // Experimental access to protected LightState data.
            if (state_exposer_->has_active_transformer()) {
                auto progress = state_exposer_->get_transformer_progress();
                auto s = state_exposer_->get_transformer_values();
                auto t = state_exposer_->get_transformer_end_values();
                //ESP_LOGD(TAG, "TRFRM %f vals [%f,%f,%f,%f,%f] new [%f,%f,%f,%f,%f]",
                //    progress,
                //    s.get_red(), s.get_green(), s.get_blue(),
                //    s.get_brightness(), s.get_color_temperature(),
                //    t.get_red(), t.get_green(), t.get_blue(),
                //    t.get_brightness(), t.get_color_temperature());
            }

            auto values = state->current_values;

            // Power down the light when its state is 'off'.
            if (values.get_state() == 0)
            {
                turn_off_();
#ifdef TRANSITION_TO_OFF_BUGFIX
                previous_state_ = -1;
                previous_brightness_ = 0;
#endif
                return;
            }

            auto brightness = values.get_brightness();

#ifdef TRANSITION_TO_OFF_BUGFIX
            // Remember the brightness that is used when the light is fully ON.
            if (values.get_state() == 1) {
                previous_brightness_ = brightness;
            }
            // When transitioning towards zero brightness ...
            else if (values.get_state() < previous_state_) {
                // ... check if the prevous brightness is the same as the current
                // brightness. If yes, then the brightness isn't being scaled ...
                if (previous_brightness_ == brightness) {
                    // ... and we need to do that ourselves.
                    brightness = values.get_state() * brightness;
                }
            }
            previous_state_ = values.get_state();
#endif

            // At the lowest brightness setting, switch to night light mode.
            // In the Yeelight integration in Home Assistant, this feature is
            // exposed trough a separate switch. I have found that the switch
            // is both confusing and made me run into issues when automating
            // the lights.
            // I don't simply check for a brightness at or below 0.01 (1%),
            // because the lowest brightness setting from Home Assistant
            // turns up as 0.011765 in here (which is 3/255).
            if (brightness < 0.012f && values.get_state() == 1) {
                turn_on_in_night_light_mode_();
                return;
            }

            // Leave it to the default tooling to figure out the basics.
            // Because of color interlocking, there are two possible outcomes:
            // - red, green, blue zero -> white light color temperature mode
            // - cwhite, wwhite zero -> RGB mode
            float red, green, blue, cwhite, wwhite;
            state->current_values_as_rgbww(
                &red, &green, &blue, &cwhite, &wwhite, true, false);

            if (cwhite > 0 || wwhite > 0) {
                turn_on_in_white_mode_(
                    values.get_color_temperature(), brightness);
            }
            else {
                turn_on_in_rgb_mode_(
                    values.get_red(), values.get_green(), values.get_blue(),
                    brightness, values.get_state());
            }
        }

    protected:
        ledc::LEDCOutput *red_;
        ledc::LEDCOutput *green_;
        ledc::LEDCOutput *blue_;
        ledc::LEDCOutput *white_;
        esphome::gpio::GPIOBinaryOutput *master1_;
        esphome::gpio::GPIOBinaryOutput *master2_;
        LightStateDataExposer *state_exposer_;
        ColorWhiteLight white_light_;
        ColorRGBLight rgb_light_;
        ColorNightLight night_light_;
#ifdef TRANSITION_TO_OFF_BUGFIX
        float previous_state_ = 1;
        float previous_brightness_ = -1;
#endif

        void turn_off_()
        {
            red_->set_level(1);
            green_->set_level(1);
            blue_->set_level(1);
            white_->set_level(0);
            master2_->turn_off();
            master1_->turn_off();
        }

        void turn_on_in_night_light_mode_()
        {
            ESP_LOGD(TAG, "Activate Night light feature");

            night_light_.set_color(1, 1, 1, 0.01, 1);

            ESP_LOGD(TAG, "New LED state : RGBW %f, %f, %f, %f", night_light_.red, night_light_.green, night_light_.blue, night_light_.white);

            // Drive the LEDs.
            master2_->turn_on();
            master1_->turn_on();
            red_->set_level(night_light_.red);
            green_->set_level(night_light_.green);
            blue_->set_level(night_light_.blue);
            white_->set_level(night_light_.white);
        }

        void turn_on_in_rgb_mode_(float red, float green, float blue, float brightness, float state)
        {
            ESP_LOGD(TAG, "Activate RGB %f, %f, %f, BRIGHTNESS %f", red, green, blue, brightness);

            rgb_light_.set_color(red, green, blue, brightness, state);

            ESP_LOGD(TAG, "New LED state : RGBW %f, %f, %f, off", rgb_light_.red, rgb_light_.green, rgb_light_.blue);

            // Drive the LEDs.
            master2_->turn_on();
            master1_->turn_on();
            red_->set_level(rgb_light_.red);
            green_->set_level(rgb_light_.green);
            blue_->set_level(rgb_light_.blue);
            white_->turn_off();
        }

        void turn_on_in_white_mode_(float temperature, float brightness)
        {
            ESP_LOGD(TAG, "Activate TEMPERATURE %f, BRIGHTNESS %f",
                temperature, brightness);

            white_light_.set_color(temperature, brightness);

            ESP_LOGD(TAG, "New LED state : RGBW %f, %f, %f, %f", 
                white_light_.red, white_light_.green, white_light_.blue,
                white_light_.white);

            master2_->turn_on();
            master1_->turn_on();
            red_->set_level(white_light_.red);
            green_->set_level(white_light_.green);
            blue_->set_level(white_light_.blue);
            white_->set_level(white_light_.white);
        }
    };

    class YeelightBS2LightState : public light::LightState, public LightStateDataExposer
    {
    public:
        YeelightBS2LightState(const std::string &name, YeelightBS2LightOutput *output) : light::LightState(name, output) {
            output->set_light_state_data_exposer(this);
        }

        bool has_active_transformer() {
            return this->transformer_ != nullptr;
        }

        light::LightColorValues get_transformer_values() {
            return this->transformer_->get_values();
        }

        light::LightColorValues get_transformer_end_values() {
            return this->transformer_->get_end_values();
        }

        float get_transformer_progress() {
            return this->transformer_->get_progress();
        }
    };
    
} // namespace bs2
} // namespace yeelight
} // namespace esphome
