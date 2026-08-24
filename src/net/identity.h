#pragma once
#include <memory>
#include <string>

namespace rivt::net {

// Device identity: a persistent P-256 key and a self-signed certificate
// (generated once, reused). Peers authenticate each other by cert
// pinning: the TLS root store is a bundle of authorized peer certs
// (each self-signed cert validates against a store containing itself).
// This is the phase-2 interim model; the signed membership log replaces
// the bundle later without changing the mechanism.
class Identity {
public:
    // Load or create under state_dir (default ~/.local/state/rivt).
    static std::unique_ptr<Identity> load_or_create(const std::string &state_dir = "");

    const std::string &key_path() const { return m_key_path; }
    const std::string &cert_path() const { return m_cert_path; }
    const std::string &cert_pem() const { return m_cert_pem; }
    // Hex SHA-256 of the certificate DER.
    const std::string &fingerprint() const { return m_fingerprint; }

    // Authorized peer bundle (default ~/.config/rivt/authorized_certs.pem).
    // Created empty if missing; returns the path.
    static std::string authorized_bundle_path(const std::string &config_dir = "");

private:
    std::string m_key_path, m_cert_path, m_cert_pem, m_fingerprint;
};

} // namespace rivt::net
