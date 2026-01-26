"""
Tests for energy monitoring functionality (HLW8012/HLW8012 pulse-based)
"""
import pytest
from client import StubProc
from conftest import Device
from tests.zcl_consts import *

HLW8012_PULSE_TIMEOUT_MS = 3000000  # 3 seconds


@pytest.fixture
def device_with_hlw8012():
    """Create a device with HLW8012 energy monitoring on pins A0 (CF), A1 (CF1), A2 (SEL)"""
    config = "TestMfr;TestModel;SA3u;RA4;EPA0A1A2;"
    with StubProc(device_config=config) as proc:
        yield Device(proc)


class TestHLW8012PulseCounting:
    """Test HLW8012 pulse counting and cluster updates"""

    def test_cf_pulses_update_power(self, device_with_hlw8012):
        """CF pin pulses should update active power attribute"""
        # Generate 10 pulses over 1 second (10Hz = ~800W with default coefficients)
        device_with_hlw8012.pulse_sequence("A0", 4, 500)
        device_with_hlw8012.step_time(5000)  # Allow processing time

        # Read power attribute
        result = device_with_hlw8012.read_zigbee_attr(
            endpoint=1,
            cluster=CLUSTER_ELECTRICAL_MEASUREMENT,
            attr=ATTR_ELEC_MEAS_ACTIVE_POWER
        )

        expected_power = 405
        assert int(result) == expected_power, f"Expected power {expected_power}, got {result}"

    def test_cf1_pulses_update_voltage_when_sel_high(self, device_with_hlw8012):
        """CF1 pin pulses should update voltage when SEL is high (default)"""
        # SEL starts high (voltage mode)
        device_with_hlw8012.pulse_sequence("A1", 4, 3000)
        device_with_hlw8012.step_time(500)  # Allow processing time

        # Read voltage attribute
        result = device_with_hlw8012.read_zigbee_attr(
            endpoint=1,
            cluster=CLUSTER_ELECTRICAL_MEASUREMENT,
            attr=ATTR_ELEC_MEAS_RMS_VOLTAGE
        )

        expected_voltage = 223
        assert int(result) == expected_voltage, f"Expected voltage {expected_voltage}, got {result}"

    def test_sel_toggles_for_current_measurement(self, device_with_hlw8012: Device):
        """SEL should toggle after interval to measure current"""
        # Step time past the SEL toggle interval
        for _ in range(10):
            device_with_hlw8012.step_time(5000 + 100)  # 5 seconds + processing time

        # Now SEL should be low (current mode)
        # Generate CF1 pulses over sample interval
        device_with_hlw8012.pulse_sequence("A1", 4, 2000)
        device_with_hlw8012.step_time(3000)

        # Read current attribute
        result = device_with_hlw8012.read_zigbee_attr(
            endpoint=1,
            cluster=CLUSTER_ELECTRICAL_MEASUREMENT,
            attr=ATTR_ELEC_MEAS_RMS_CURRENT
        )

        expected_current = 10
        assert int(result) == expected_current, f"Expected current {expected_current}, got {result}"

    def test_no_pulses_timeout_zeros_power(self, device_with_hlw8012):
        """Power should go to 0 if no pulses received within timeout"""
        device_with_hlw8012.pulse_sequence("A0", 4, 1000)
        device_with_hlw8012.step_time(4000)

        # Verify power is set
        result1 = device_with_hlw8012.read_zigbee_attr(
            endpoint=1,
            cluster=CLUSTER_ELECTRICAL_MEASUREMENT,
            attr=ATTR_ELEC_MEAS_ACTIVE_POWER
        )
        assert int(result1) > 0, "Power should be non-zero after pulses"

        # Wait for timeout
        device_with_hlw8012.step_time(HLW8012_PULSE_TIMEOUT_MS)
        device_with_hlw8012.step_time(5000)

        # Power should now be 0
        result2 = device_with_hlw8012.read_zigbee_attr(
            endpoint=1,
            cluster=CLUSTER_ELECTRICAL_MEASUREMENT,
            attr=ATTR_ELEC_MEAS_ACTIVE_POWER
        )
        assert int(result2) == 0, f"Power should be 0 after timeout, got {result2}"

    def test_multiple_measurements_update_sequentially(self, device_with_hlw8012):
        """Multiple pulse measurements should update values sequentially"""
        # First measurement with 10 pulses over 1 second (10Hz)
        device_with_hlw8012.pulse_sequence("A0", 8, 500)
        device_with_hlw8012.step_time(4500)

        power1 = int(device_with_hlw8012.read_zigbee_attr(
            endpoint=1,
            cluster=CLUSTER_ELECTRICAL_MEASUREMENT,
            attr=ATTR_ELEC_MEAS_ACTIVE_POWER
        ))

        # Second measurement with 20 pulses over 1 second (20Hz = higher power)
        device_with_hlw8012.pulse_sequence("A0", 8, 1000)
        device_with_hlw8012.step_time(4000)

        power2 = int(device_with_hlw8012.read_zigbee_attr(
            endpoint=1,
            cluster=CLUSTER_ELECTRICAL_MEASUREMENT,
            attr=ATTR_ELEC_MEAS_ACTIVE_POWER
        ))

        # Second measurement with 30 pulses over 1 second (30Hz = much higher power)
        device_with_hlw8012.pulse_sequence("A0", 8, 1500)
        device_with_hlw8012.step_time(3500)

        power3 = int(device_with_hlw8012.read_zigbee_attr(
            endpoint=1,
            cluster=CLUSTER_ELECTRICAL_MEASUREMENT,
            attr=ATTR_ELEC_MEAS_ACTIVE_POWER
        ))

        assert power1 == 405
        assert power2 == 810
        assert power3 == 1215
        assert power2 > power1, "Higher frequency should mean higher power"


class TestEnergyMonitoringClusterPresence:
    """Test that energy monitoring clusters are present when configured"""

    def test_electrical_measurement_cluster_present_with_hlw8012(self, device_with_hlw8012):
        """ElectricalMeasurement cluster (0x0B04) should be on endpoint 1"""
        # Read the measurement type attribute to verify cluster exists
        result = device_with_hlw8012.read_zigbee_attr(
            endpoint=1,
            cluster=CLUSTER_ELECTRICAL_MEASUREMENT,
            attr=ATTR_ELEC_MEAS_MEASUREMENT_TYPE
        )
        # Measurement type should indicate AC active measurement (bit 0 set)
        assert result is not None
        assert int(result) & 0x01  # AC active measurement bit

    def test_metering_cluster_present_with_hlw8012(self, device_with_hlw8012):
        """Metering cluster (0x0702) should be on endpoint 1"""
        result = device_with_hlw8012.read_zigbee_attr(
            endpoint=1,
            cluster=CLUSTER_METERING,
            attr=ATTR_METERING_UNIT_OF_MEASURE
        )
        # Unit of measure should be kWh (0x00)
        assert result == "0"


class TestElectricalMeasurementAttributes:
    """Test ElectricalMeasurement cluster attributes"""

    def test_read_rms_voltage(self, device_with_hlw8012):
        """Should be able to read RMS voltage attribute"""
        result = device_with_hlw8012.read_zigbee_attr(
            endpoint=1,
            cluster=CLUSTER_ELECTRICAL_MEASUREMENT,
            attr=ATTR_ELEC_MEAS_RMS_VOLTAGE
        )
        # Voltage should be 0 initially (no measurement yet)
        assert result == "0"

    def test_read_rms_current(self, device_with_hlw8012):
        """Should be able to read RMS current attribute"""
        result = device_with_hlw8012.read_zigbee_attr(
            endpoint=1,
            cluster=CLUSTER_ELECTRICAL_MEASUREMENT,
            attr=ATTR_ELEC_MEAS_RMS_CURRENT
        )
        assert result == "0"

    def test_read_active_power(self, device_with_hlw8012):
        """Should be able to read active power attribute"""
        result = device_with_hlw8012.read_zigbee_attr(
            endpoint=1,
            cluster=CLUSTER_ELECTRICAL_MEASUREMENT,
            attr=ATTR_ELEC_MEAS_ACTIVE_POWER
        )
        assert result == "0"

    def test_read_voltage_multiplier_divisor(self, device_with_hlw8012):
        """Should be able to read voltage multiplier and divisor"""
        mult_result = device_with_hlw8012.read_zigbee_attr(
            endpoint=1,
            cluster=CLUSTER_ELECTRICAL_MEASUREMENT,
            attr=ATTR_ELEC_MEAS_AC_VOLTAGE_MULTIPLIER
        )
        div_result = device_with_hlw8012.read_zigbee_attr(
            endpoint=1,
            cluster=CLUSTER_ELECTRICAL_MEASUREMENT,
            attr=ATTR_ELEC_MEAS_AC_VOLTAGE_DIVISOR
        )
        # Default: multiplier=1, divisor=10 (for 0.1V units)
        assert mult_result == "1"
        assert div_result == "10"

    def test_read_cf_freq(self, device_with_hlw8012):
        """Should be able to read active power attribute"""
        # First measurement with 10 pulses over 1 second (10Hz)
        device_with_hlw8012.pulse_sequence("A0", 4, 1000)
        device_with_hlw8012.step_time(5000)

        result = device_with_hlw8012.read_zigbee_attr(
            endpoint=1,
            cluster=CLUSTER_ELECTRICAL_MEASUREMENT,
            attr=0xff00
        )
        assert result == "2000"


class TestMeteringAttributes:
    """Test Metering cluster attributes"""

    def test_read_current_summation_delivered(self, device_with_hlw8012):
        """Should be able to read accumulated energy"""
        result = device_with_hlw8012.read_zigbee_attr(
            endpoint=1,
            cluster=CLUSTER_METERING,
            attr=ATTR_METERING_CURRENT_SUMMATION_DELIVERED
        )
        # Energy should be 0 initially
        assert result == "0"

        for _ in range(10):
            device_with_hlw8012.pulse_sequence("A0", 1, 5000)

        result_after = device_with_hlw8012.read_zigbee_attr(
            endpoint=1,
            cluster=CLUSTER_METERING,
            attr=ATTR_METERING_CURRENT_SUMMATION_DELIVERED
        )

        assert int(result_after) == 45
        assert int(result_after) > 0, "Energy should have increased after pulses"

    def test_read_multiplier_divisor(self, device_with_hlw8012):
        """Should be able to read multiplier and divisor for unit conversion"""
        mult_result = device_with_hlw8012.read_zigbee_attr(
            endpoint=1,
            cluster=CLUSTER_METERING,
            attr=ATTR_METERING_MULTIPLIER
        )
        div_result = device_with_hlw8012.read_zigbee_attr(
            endpoint=1,
            cluster=CLUSTER_METERING,
            attr=ATTR_METERING_DIVISOR
        )
        # Default: multiplier=1, divisor=1000 (Wh to kWh)
        assert mult_result == "1"
        assert div_result == "1000"

    def test_read_metering_device_type(self, device_with_hlw8012):
        """Metering device type should be electric (0x00)"""
        result = device_with_hlw8012.read_zigbee_attr(
            endpoint=1,
            cluster=CLUSTER_METERING,
            attr=ATTR_METERING_METERING_DEVICE_TYPE
        )
        assert result == "00"  # Electric metering
