"""Validate imageproxy ingress configuration without starting containers."""

import json
import os
from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]
SERVICE = ROOT / "services" / "imageproxy"


def compose_config(filename, **variables):
    env = os.environ.copy()
    for name in ("IMAGE_PROXY_DOMAIN", "IMAGE_PROXY_FUNNEL_HOST",
                 "IMAGE_PROXY_KEY", "IMAGE_PROXY_KEY_ID"):
        env.pop(name, None)
    env.update(variables)
    return subprocess.run(
        ["docker", "compose", "--env-file", "/dev/null", "-f",
         str(SERVICE / filename), "config", "--format", "json"],
        env=env, capture_output=True, text=True, check=False,
    )


class ImageproxyDeploymentTests(unittest.TestCase):
    def test_funnel_relay_publishes_only_loopback_and_targets_traefik(self):
        result = compose_config(
            "compose.dokploy-funnel.yml",
            IMAGE_PROXY_FUNNEL_HOST="image-server.example.ts.net",
            IMAGE_PROXY_KEY="test-only-key",
            IMAGE_PROXY_KEY_ID="test-key-id",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        services = json.loads(result.stdout)["services"]
        self.assertIn("funnel-relay", services)
        relay = services["funnel-relay"]
        ports = relay["ports"]
        self.assertEqual(len(ports), 1)
        self.assertEqual(ports[0]["host_ip"], "127.0.0.1")
        self.assertEqual(ports[0]["published"], "8091")
        self.assertEqual(ports[0]["target"], 8091)
        self.assertEqual(ports[0]["protocol"], "tcp")
        self.assertNotIn("network_mode", relay)
        self.assertIn("dokploy-network", relay["networks"])
        self.assertEqual(relay["restart"], "unless-stopped")
        self.assertEqual(relay["labels"]["traefik.enable"], "false")
        self.assertEqual(relay["command"], [
            "TCP4-LISTEN:8091,bind=0.0.0.0,reuseaddr,fork,max-children=64",
            "TCP4-CONNECT:dokploy-traefik:8091,connect-timeout=5",
        ])
        self.assertEqual(relay["user"], "65534:65534")
        self.assertTrue(relay["read_only"])
        self.assertEqual(relay["cap_drop"], ["ALL"])
        self.assertIn("no-new-privileges:true", relay["security_opt"])
        self.assertTrue(relay["image"].startswith("alpine/socat:1.8.0.3@sha256:"))
        self.assertEqual(sum(len(service.get("ports", []))
                             for service in services.values()), 1)

    def test_funnel_preserves_controlled_ingress(self):
        result = compose_config(
            "compose.dokploy-funnel.yml",
            IMAGE_PROXY_FUNNEL_HOST="image-server.example.ts.net",
            IMAGE_PROXY_KEY="test-only-key",
            IMAGE_PROXY_KEY_ID="test-key-id",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        service = json.loads(result.stdout)["services"]["imageproxy"]
        self.assertFalse(service.get("ports"))
        self.assertNotIn("network_mode", service)
        self.assertEqual(service["expose"], ["8090"])
        self.assertIn("dokploy-network", service["networks"])
        self.assertEqual(service["environment"]["IMAGE_PROXY_ADDR"], ":8090")
        self.assertEqual(service["environment"]["IMAGE_PROXY_KEY"], "test-only-key")
        self.assertEqual(service["environment"]["IMAGE_PROXY_KEY_ID"], "test-key-id")
        labels = service["labels"]
        router = "traefik.http.routers.imageproxy-funnel."
        self.assertEqual(labels[router + "rule"], "Host(`image-server.example.ts.net`)")
        self.assertEqual(labels[router + "entrypoints"], "funnel")
        self.assertEqual(labels[router + "service"], "vitashelflauncher-imageproxy")
        self.assertEqual(labels[router + "middlewares"], "vitashelflauncher-imageproxy-ratelimit")
        self.assertFalse(any(".tls" in key for key in labels))
        middleware = "traefik.http.middlewares.vitashelflauncher-imageproxy-ratelimit.ratelimit."
        self.assertEqual(labels[middleware + "average"], "300")
        self.assertEqual(labels[middleware + "period"], "1m")
        self.assertEqual(labels[middleware + "burst"], "50")
        self.assertEqual(labels["traefik.http.services.vitashelflauncher-imageproxy.loadbalancer.server.port"], "8090")

    def test_funnel_requires_hostname_and_authentication(self):
        variables = dict(
            IMAGE_PROXY_FUNNEL_HOST="image-server.example.ts.net",
            IMAGE_PROXY_KEY="test-only-key",
            IMAGE_PROXY_KEY_ID="test-key-id",
        )
        for name in variables:
            for missing in (None, ""):
                with self.subTest(variable=name, value=missing):
                    env = variables.copy()
                    if missing is None:
                        env.pop(name)
                    else:
                        env[name] = missing
                    result = compose_config("compose.dokploy-funnel.yml", **env)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(name, result.stderr)

    def test_existing_https_deployment_is_unchanged(self):
        result = compose_config("compose.dokploy.yml", IMAGE_PROXY_DOMAIN="images.example.com")
        self.assertEqual(result.returncode, 0, result.stderr)
        service = json.loads(result.stdout)["services"]["imageproxy"]
        self.assertFalse(service.get("ports"))
        self.assertEqual(service["environment"]["IMAGE_PROXY_KEY"], "")
        self.assertEqual(service["environment"]["IMAGE_PROXY_KEY_ID"], "")
        labels = service["labels"]
        router = "traefik.http.routers.vitashelflauncher-imageproxy."
        self.assertEqual(labels[router + "entrypoints"], "websecure")
        self.assertEqual(labels[router + "rule"], "Host(`images.example.com`)")
        self.assertEqual(labels[router + "tls.certresolver"], "letsencrypt")
        self.assertEqual(labels[router + "middlewares"], "vitashelflauncher-imageproxy-ratelimit")


if __name__ == "__main__":
    unittest.main()
