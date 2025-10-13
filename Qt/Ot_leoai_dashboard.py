"""High level data access layer for the Ot LeoAI energy dashboard.

The dashboard ships with three default overview pages that mirror the
EnergyWise product concept:

* **Sivu 1 — Markkinakatsaus:** highlights the Nord Pool spot prices together
  with current Fingrid measurements.
* **Sivu 2 — Ennusteet:** presents the in-house 48–72 hour forecast series and
  overlays key Nord Pool reference values.
* **Sivu 3 — Euroopan markkinadata:** aggregates ENTSO-E transparency data for
  a broader European perspective.

This module centralises access to the external data providers used by the
EnergyWise toolkit.  It focuses on the data sources mentioned in the product
summary:

* Nord Pool API for 24 hour price information.
* Fingrid API for automatic 24 hour measurements.
* An internal AI model that produces 48–72 hour forecasts.
* ENTSO-E Transparency platform for 1–7 day market data.

The classes expose small, well documented methods that return normalised data
structures.  They are suitable for both Qt based graphical clients and
headless command line tools.
"""

from __future__ import annotations

import csv
import datetime as dt
import io
import json
import logging
import os
import textwrap
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from typing import Any, Dict, Iterable, List, Optional, Sequence, TextIO, Union, cast


CSVSource = Union[str, os.PathLike[str], TextIO]

_LOGGER = logging.getLogger(__name__)


class EnergyAPIError(RuntimeError):
    """Raised when a remote energy data provider returns an unexpected result."""


@dataclass(slots=True)
class EnergyDataPoint:
    """Container for a time series value used by the dashboard."""

    timestamp: dt.datetime
    value: float
    metadata: Dict[str, Any] = field(default_factory=dict)

    def as_dict(self) -> Dict[str, Any]:
        """Return the point as a serialisable dictionary."""

        payload = {"timestamp": self.timestamp.isoformat(), "value": self.value}
        if self.metadata:
            payload["metadata"] = self.metadata
        return payload


@dataclass(slots=True)
class DashboardPage:
    """Structure describing one of the initial dashboard views."""

    title: str
    summary: str
    data_sources: Sequence[str]

    def as_dict(self) -> Dict[str, Any]:
        """Return a serialisable representation useful for Qt models."""

        return {
            "title": self.title,
            "summary": self.summary,
            "data_sources": list(self.data_sources),
        }


def _default_dashboard_pages() -> List[DashboardPage]:
    """Return the built-in three page layout requested for the UI shell."""

    return [
        DashboardPage(
            title="Sivu 1 — Markkinakatsaus",
            summary="Nord Pool -hinnat ja Fingridin ajantasaiset mittaukset yhdellä näkymällä.",
            data_sources=("Nord Pool", "Fingrid"),
        ),
        DashboardPage(
            title="Sivu 2 — Ennusteet",
            summary="Sisäinen AI-ennuste 48–72 tunniksi eteenpäin yhdessä hintareferenssien kanssa.",
            data_sources=("Oma malli (AI)", "Nord Pool"),
        ),
        DashboardPage(
            title="Sivu 3 — Euroopan markkinadata",
            summary="ENTSO-E:n läpinäkyvyysalustan 1–7 vrk markkina-analyysi.",
            data_sources=("ENTSO-E",),
        ),
    ]


def _http_get(url: str, *, headers: Optional[Dict[str, str]] = None, params: Optional[Dict[str, str]] = None, timeout: float = 10.0) -> bytes:
    """HTTP helper that avoids a hard dependency on the `requests` package."""

    if params:
        query = urllib.parse.urlencode(params)
        separator = "&" if urllib.parse.urlparse(url).query else "?"
        url = f"{url}{separator}{query}"

    request = urllib.request.Request(url, headers=headers or {})
    with urllib.request.urlopen(request, timeout=timeout) as response:  # noqa: S310 - remote APIs are trusted here.
        return response.read()


def _parse_timestamp(value: str) -> dt.datetime:
    """Parse ISO 8601 timestamps returned by the remote APIs."""

    return dt.datetime.fromisoformat(value.replace("Z", "+00:00"))


class NordPoolClient:
    """Utility for downloading hourly prices from the Nord Pool API."""

    _BASE_URL = "https://www.nordpoolgroup.com/api/marketdata/page/35"

    def __init__(self, *, timeout: float = 10.0) -> None:
        self._timeout = timeout

    def fetch_hourly_prices(self, area: str = "FI", *, currency: str = "EUR") -> List[EnergyDataPoint]:
        """Fetch hourly spot prices for the given bidding area.

        The Nord Pool JSON schema contains a list of rows, each with columns for
        the bidding areas.  The helper extracts the column that matches
        ``area`` and normalises the values into :class:`EnergyDataPoint`
        instances.
        """

        raw = _http_get(self._BASE_URL, params={"currency": currency}, timeout=self._timeout)
        payload = json.loads(raw)
        data = payload.get("data", {})
        rows = data.get("Rows", []) or data.get("rows", [])

        prices: List[EnergyDataPoint] = []
        for row in rows:
            try:
                start_time = _parse_timestamp(row["StartTime"])
            except (KeyError, ValueError) as exc:  # pragma: no cover - defensive guard.
                _LOGGER.debug("Skipping Nord Pool row without StartTime: %s", exc)
                continue

            for column in row.get("Columns", []):
                if column.get("Name") != area:
                    continue
                value_str = column.get("Value")
                if not value_str:
                    continue
                normalised = float(value_str.replace(",", "."))
                prices.append(
                    EnergyDataPoint(
                        timestamp=start_time,
                        value=normalised,
                        metadata={
                            "end_time": row.get("EndTime"),
                            "unit": data.get("Unit"),
                            "area": area,
                        },
                    )
                )
        return prices


class FingridClient:
    """Wrapper around the Fingrid open data API."""

    _BASE_URL = "https://api.fingrid.fi/v1/variable/248/events/json"

    def __init__(self, *, api_key: Optional[str] = None, timeout: float = 10.0) -> None:
        self._api_key = api_key
        self._timeout = timeout

    def fetch_automatic_measurements(self, *, start_time: Optional[dt.datetime] = None) -> List[EnergyDataPoint]:
        """Fetch the most recent balance power measurements from Fingrid."""

        headers = {"Accept": "application/json"}
        if self._api_key:
            headers["x-api-key"] = self._api_key
        params: Dict[str, str] = {}
        if start_time:
            params["start_time"] = start_time.isoformat()
        raw = _http_get(self._BASE_URL, headers=headers, params=params, timeout=self._timeout)
        payload = json.loads(raw)

        measurements: List[EnergyDataPoint] = []
        for entry in payload:
            try:
                timestamp = _parse_timestamp(entry["start_time"])
                value = float(entry["value"])
            except (KeyError, ValueError) as exc:  # pragma: no cover - defensive guard.
                _LOGGER.debug("Skipping Fingrid entry: %s", exc)
                continue
            measurements.append(
                EnergyDataPoint(
                    timestamp=timestamp,
                    value=value,
                    metadata={"end_time": entry.get("end_time"), "source": "Fingrid"},
                )
            )
        return measurements


class EntsoeClient:
    """Small helper for the ENTSO-E transparency platform."""

    _BASE_URL = "https://transparency.entsoe.eu/api"

    def __init__(self, api_key: str, *, timeout: float = 10.0) -> None:
        if not api_key:
            raise ValueError("ENTSO-E requires a valid API key.")
        self._api_key = api_key
        self._timeout = timeout

    def fetch_day_ahead_prices(self, *, domain: str, period_start: dt.datetime, period_end: dt.datetime) -> List[EnergyDataPoint]:
        """Fetch day-ahead prices for the requested EIC domain.

        Parameters are forwarded to the ENTSO-E API as described in the
        transparency documentation.  Responses are XML documents that contain a
        list of time series.  The method consolidates those into a flat list of
        :class:`EnergyDataPoint` instances.
        """

        params = {
            "securityToken": self._api_key,
            "documentType": "A44",  # Day-ahead prices
            "in_Domain": domain,
            "out_Domain": domain,
            "periodStart": period_start.strftime("%Y%m%d%H%M"),
            "periodEnd": period_end.strftime("%Y%m%d%H%M"),
        }
        raw = _http_get(f"{self._BASE_URL}", params=params, timeout=self._timeout)
        return self._parse_price_xml(raw)

    def _parse_price_xml(self, payload: bytes) -> List[EnergyDataPoint]:
        try:
            document = ET.fromstring(payload)
        except ET.ParseError as exc:  # pragma: no cover - defensive guard.
            raise EnergyAPIError("Failed to parse ENTSO-E response") from exc

        namespace = {"ns": "urn:iec62325.351:tc57wg16:451-3:publicationdocument:7:0"}
        series_nodes = document.findall(".//ns:TimeSeries", namespace)
        prices: List[EnergyDataPoint] = []
        for series in series_nodes:
            period = series.find("ns:Period", namespace)
            if period is None:
                continue
            start_str = period.findtext("ns:timeInterval/ns:start", default="", namespaces=namespace)
            resolution = period.findtext("ns:resolution", default="PT60M", namespaces=namespace)
            if not start_str:
                continue
            start_time = _parse_timestamp(start_str)
            duration = _duration_from_resolution(resolution)
            for index, point in enumerate(period.findall("ns:Point", namespace)):
                position = point.findtext("ns:position", default="", namespaces=namespace)
                price_str = point.findtext("ns:price.amount", default="", namespaces=namespace)
                if not position or not price_str:
                    continue
                offset = (int(position) - 1) * duration
                prices.append(
                    EnergyDataPoint(
                        timestamp=start_time + offset,
                        value=float(price_str),
                        metadata={"resolution": resolution, "source": "ENTSO-E"},
                    )
                )
        return prices


def _duration_from_resolution(resolution: str) -> dt.timedelta:
    """Convert ISO 8601 durations (e.g. ``PT60M``) to :class:`datetime.timedelta`."""

    if resolution == "PT60M":
        return dt.timedelta(hours=1)
    if resolution == "PT30M":
        return dt.timedelta(minutes=30)
    if resolution == "PT15M":
        return dt.timedelta(minutes=15)
    if resolution == "PT1H":  # Some endpoints use the expanded form.
        return dt.timedelta(hours=1)
    if resolution == "P1D":
        return dt.timedelta(days=1)
    raise EnergyAPIError(f"Unsupported resolution: {resolution}")


class ForecastSeries:
    """Represents locally generated forecast data."""

    def __init__(self, points: Sequence[EnergyDataPoint], *, description: str = "") -> None:
        self._points = list(points)
        self.description = description

    @property
    def points(self) -> Sequence[EnergyDataPoint]:
        return tuple(self._points)

    def as_dict(self) -> Dict[str, Any]:
        return {
            "description": self.description,
            "points": [point.as_dict() for point in self._points],
        }


class ForecastLoader:
    """Utility helpers for loading internal AI forecast results."""

    def load_from_csv(self, source: CSVSource) -> ForecastSeries:
        """Load forecast data from a CSV file.

        The CSV is expected to contain ``timestamp`` and ``value`` columns.
        Additional columns are stored in the metadata dictionary.
        """

        handle: TextIO
        close_handle = False
        if isinstance(source, io.TextIOBase):
            handle = source
        elif hasattr(source, "read"):
            handle = cast(TextIO, source)
        else:
            handle = open(os.fspath(source), "r", encoding="utf-8")
            close_handle = True

        points: List[EnergyDataPoint] = []
        try:
            reader = csv.DictReader(handle)
            for row in reader:
                timestamp = _parse_timestamp(row["timestamp"])
                value = float(row["value"])
                metadata = {key: row[key] for key in row if key not in {"timestamp", "value"}}
                points.append(EnergyDataPoint(timestamp=timestamp, value=value, metadata=metadata))
        finally:
            if close_handle:
                handle.close()

        description = "Forecast from CSV stream"
        if isinstance(source, (str, os.PathLike)):
            description = f"Forecast from {os.path.basename(os.fspath(source))}"
        return ForecastSeries(points, description=description)

    def load_from_json(self, payload: str) -> ForecastSeries:
        """Load forecast data from a JSON string."""

        data = json.loads(payload)
        points: List[EnergyDataPoint] = []
        for entry in data:
            timestamp = _parse_timestamp(entry["timestamp"])
            value = float(entry["value"])
            metadata = {key: entry[key] for key in entry if key not in {"timestamp", "value"}}
            points.append(EnergyDataPoint(timestamp=timestamp, value=value, metadata=metadata))
        return ForecastSeries(points, description="Forecast loaded from JSON payload")


class OtLeoAIDashboard:
    """Facade that offers a coherent interface over the different providers."""

    def __init__(
        self,
        *,
        fingrid_api_key: Optional[str] = None,
        entsoe_api_key: Optional[str] = None,
        http_timeout: float = 10.0,
    ) -> None:
        self.nord_pool = NordPoolClient(timeout=http_timeout)
        self.fingrid = FingridClient(api_key=fingrid_api_key, timeout=http_timeout)
        self.entsoe: Optional[EntsoeClient] = None
        if entsoe_api_key:
            self.entsoe = EntsoeClient(entsoe_api_key, timeout=http_timeout)
        self.forecast_loader = ForecastLoader()
        self._forecast: Optional[ForecastSeries] = None
        self._pages: Optional[List[DashboardPage]] = None

    @property
    def pages(self) -> Sequence[DashboardPage]:
        """Return the three default pages, initialising them lazily."""

        if self._pages is None:
            self._pages = _default_dashboard_pages()
        return tuple(self._pages)

    def override_pages(self, pages: Iterable[DashboardPage]) -> None:
        """Allow the Qt front-end to provide a custom page order."""

        self._pages = list(pages)

    def load_forecast_csv(self, path: str) -> ForecastSeries:
        """Load an internally generated forecast and cache it for repeated use."""

        forecast = self.forecast_loader.load_from_csv(path)
        self._forecast = forecast
        return forecast

    def load_forecast_json(self, payload: str) -> ForecastSeries:
        forecast = self.forecast_loader.load_from_json(payload)
        self._forecast = forecast
        return forecast

    def summary(self) -> str:
        """Return a human readable summary of the available data sources."""

        header = ("Tavoite", "Tapa", "Ennustehorisontti", "Työkalu/API")
        rows = [
            (
                "Tarkat hinnat",
                "Nord Pool API",
                "24 h",
                "https://www.nordpoolgroup.com/api/marketdata/page/35",
            ),
            (
                "Automaattinen haku",
                "Fingrid API",
                "24 h",
                "https://api.fingrid.fi/v1/variable/248/events/json",
            ),
            (
                "Ennuste 2–3 vrk",
                "Oma malli (AI)",
                "48–72 h",
                "Python, TensorFlow, EnergyWise agent",
            ),
            (
                "Markkinadata",
                "ENTSO-E",
                "1–7 vrk",
                "https://transparency.entsoe.eu",
            ),
        ]
        widths = [max(len(column), *(len(row[index]) for row in rows)) for index, column in enumerate(header)]
        lines = ["🔹 5. Yhteenveto"]
        header_line = "  ".join(column.ljust(widths[idx]) for idx, column in enumerate(header))
        lines.append(header_line)
        for row in rows:
            lines.append("  ".join(value.ljust(widths[idx]) for idx, value in enumerate(row)))
        return "\n".join(lines)

    def fetch_latest_prices(self, area: str = "FI") -> List[EnergyDataPoint]:
        return self.nord_pool.fetch_hourly_prices(area=area)

    def fetch_latest_measurements(self) -> List[EnergyDataPoint]:
        return self.fingrid.fetch_automatic_measurements()

    def fetch_entsoe_day_ahead(self, domain: str, period_start: dt.datetime, period_end: dt.datetime) -> List[EnergyDataPoint]:
        if not self.entsoe:
            raise EnergyAPIError("ENTSO-E client is not initialised; provide an API key when constructing OtLeoAIDashboard")
        return self.entsoe.fetch_day_ahead_prices(domain=domain, period_start=period_start, period_end=period_end)

    def get_cached_forecast(self) -> Optional[ForecastSeries]:
        return self._forecast


def load_forecast_from_text(text: str) -> ForecastSeries:
    """Convenience helper that detects CSV versus JSON payloads automatically."""

    stripped = text.lstrip()
    loader = ForecastLoader()
    if stripped.startswith("["):
        return loader.load_from_json(text)
    return loader.load_from_csv(io.StringIO(text))


__all__ = [
    "EnergyAPIError",
    "EnergyDataPoint",
    "DashboardPage",
    "NordPoolClient",
    "FingridClient",
    "EntsoeClient",
    "ForecastSeries",
    "ForecastLoader",
    "OtLeoAIDashboard",
    "load_forecast_from_text",
]
