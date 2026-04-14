use crate::types::NodeGeo;
use tracing::{info, warn};

/// Look up geographic location from an IP address using ip-api.com (free tier).
/// Returns None on failure — node still registers, just without geo data.
pub async fn lookup_ip(ip: &str) -> Option<NodeGeo> {
    let url = format!("http://ip-api.com/json/{}?fields=lat,lon,city,country,regionName,isp", ip);

    let resp = match reqwest::get(&url).await {
        Ok(r) => r,
        Err(e) => {
            warn!("GeoIP lookup failed for {}: {}", ip, e);
            return None;
        }
    };

    let json: serde_json::Value = match resp.json().await {
        Ok(j) => j,
        Err(e) => {
            warn!("GeoIP parse failed for {}: {}", ip, e);
            return None;
        }
    };

    let lat = json["lat"].as_f64()?;
    let lng = json["lon"].as_f64()?;
    let city = json["city"].as_str().unwrap_or("Unknown").to_string();
    let country = json["country"].as_str().unwrap_or("Unknown").to_string();
    let region = json["regionName"].as_str().unwrap_or("Unknown").to_string();
    let isp = json["isp"].as_str().unwrap_or("Unknown").to_string();

    info!("GeoIP: {} → {}, {} ({:.2}, {:.2})", ip, city, country, lat, lng);

    Some(NodeGeo {
        lat,
        lng,
        city,
        country,
        region,
        isp,
    })
}

/// Haversine distance in kilometers between two (lat, lng) points.
pub fn haversine_km(lat1: f64, lng1: f64, lat2: f64, lng2: f64) -> f64 {
    let r = 6371.0; // Earth radius in km
    let d_lat = (lat2 - lat1).to_radians();
    let d_lng = (lng2 - lng1).to_radians();
    let a = (d_lat / 2.0).sin().powi(2)
        + lat1.to_radians().cos() * lat2.to_radians().cos() * (d_lng / 2.0).sin().powi(2);
    let c = 2.0 * a.sqrt().asin();
    r * c
}
