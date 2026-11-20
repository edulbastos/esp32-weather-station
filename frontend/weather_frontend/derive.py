"""Values derived from a reading with the formulas of the station spec."""

from __future__ import annotations

import math

# Operating range of the BME280 (station FR-1.6, SENSOR_MIN_*/SENSOR_MAX_* of main/config.h).
TEMP_RANGE_C = (-40.0, 85.0)
HUM_RANGE_RH = (0.0, 100.0)
PRESS_RANGE_HPA = (300.0, 1100.0)


def msl_factor(altitude_m: float) -> float:
    """Station FR-11.2: reduction to sea level with the ICAO standard atmosphere."""
    return (1.0 - 2.25577e-5 * altitude_m) ** -5.25588


def dew_point(temp_c: float, hum_rh: float) -> float | None:
    """Station FR-11.3: Magnus formula, constants of Sonntag (1990)."""
    if hum_rh <= 0 or temp_c < -45 or temp_c > 60:
        return None
    b, c = 17.62, 243.12
    gamma = math.log(hum_rh / 100.0) + b * temp_c / (c + temp_c)
    return c * gamma / (b - gamma)


def in_range(temp_c: float, hum_rh: float, press_hpa: float) -> bool:
    return (
        TEMP_RANGE_C[0] <= temp_c <= TEMP_RANGE_C[1]
        and HUM_RANGE_RH[0] <= hum_rh <= HUM_RANGE_RH[1]
        and PRESS_RANGE_HPA[0] <= press_hpa <= PRESS_RANGE_HPA[1]
    )
