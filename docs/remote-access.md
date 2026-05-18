# Remote Access

Access your OneCam dashboard from anywhere in the world - your phone on mobile data, a laptop at a coffee shop, or sharing a link with a family member.

This guide uses **Cloudflare Tunnel** (`cloudflared`). It is free, requires no port forwarding, no static IP, and works even if your ISP uses CGNAT (which blocks inbound connections entirely).

## How it works

`cloudflared` creates an outbound encrypted connection from your server to Cloudflare's edge network. Cloudflare then serves your dashboard publicly over HTTPS. Your router never needs to open a port.

```
Browser (anywhere)
    ↕ HTTPS
Cloudflare edge
    ↕ encrypted tunnel (outbound from your machine)
cloudflared process on your server
    ↕ HTTP localhost
OneCam FastAPI server
    ↕ HTTP
ESP32-CAM
```

---

## Option A - Quick test (no account needed)

This gives you a random public URL that lasts until you stop the process. Good for testing.

```bash
# Install cloudflared (Debian/Ubuntu)
curl -L https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-amd64 \
     -o /usr/local/bin/cloudflared
chmod +x /usr/local/bin/cloudflared

# Start the server
uv run uvicorn main:app --host 0.0.0.0 --port 8000

# In another terminal - create a temporary tunnel
cloudflared tunnel --url http://localhost:8000
```

Output:
```
+--------------------------------------------------------------------------------------------+
|  Your quick Tunnel has been created! Visit it at (it may take some time to be reachable): |
|  https://random-words-here.trycloudflare.com                                               |
+--------------------------------------------------------------------------------------------+
```

Open that URL from any device, anywhere.

---

## Option B - Permanent tunnel with your own domain (free)

This gives you a fixed URL like `onecam.yourdomain.com` that works even after reboots.

### Prerequisites

- A free [Cloudflare account](https://dash.cloudflare.com/sign-up)
- A domain added to Cloudflare (even a cheap one from Namecheap/Porkbun works)

### 1 - Install and authenticate

```bash
# Install cloudflared
curl -L https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-amd64 \
     -o /usr/local/bin/cloudflared
chmod +x /usr/local/bin/cloudflared

# Log in (opens a browser window)
cloudflared tunnel login
```

### 2 - Create a named tunnel

```bash
cloudflared tunnel create onecam
```

This creates a tunnel and saves a credentials file at `~/.cloudflared/<tunnel-id>.json`. Note the tunnel ID printed in the output.

### 3 - Create the config file

Create `~/.cloudflared/config.yml`:

```yaml
tunnel: <your-tunnel-id>
credentials-file: /home/<your-username>/.cloudflared/<your-tunnel-id>.json

ingress:
  - hostname: onecam.yourdomain.com
    service: http://localhost:8000
  - service: http_status:404
```

Replace `<your-tunnel-id>`, `<your-username>`, and `yourdomain.com` accordingly.

### 4 - Point DNS to the tunnel

```bash
cloudflared tunnel route dns onecam onecam.yourdomain.com
```

This adds a CNAME record in Cloudflare DNS automatically.

### 5 - Run the tunnel

```bash
cloudflared tunnel run onecam
```

Your dashboard is now live at `https://onecam.yourdomain.com`.

### 6 - Make it permanent (systemd)

```bash
# Install as a systemd service
sudo cloudflared service install

# Start and enable
sudo systemctl start cloudflared
sudo systemctl enable cloudflared

# Check status
sudo systemctl status cloudflared
```

The tunnel now starts automatically on boot, even before you log in.

---

## Option C - Run on a Raspberry Pi

If you want the Python server to run 24/7 without keeping a laptop on, a Raspberry Pi (any model) works well as a dedicated host.

```bash
# Install dependencies on Pi
uv sync

# Run with systemd (create /etc/systemd/system/onecam.service):
[Unit]
Description=OneCam home monitoring server
After=network.target

[Service]
User=pi
WorkingDirectory=/home/pi/OneCam
ExecStart=uv run uvicorn main:app --host 0.0.0.0 --port 8000
Restart=always

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable onecam
sudo systemctl start onecam
```

Combine with cloudflared systemd service (Option B step 6) for a fully autonomous setup.

---

## Adding authentication

The Cloudflare dashboard lets you add **Cloudflare Access** - a zero-trust auth gate in front of your tunnel. When enabled, anyone visiting your URL must log in with their Google, GitHub, or email account before reaching the dashboard.

To enable:
1. Go to Cloudflare dashboard → **Zero Trust** → **Access** → **Applications**
2. Click **Add an application** → **Self-hosted**
3. Set the application domain to `onecam.yourdomain.com`
4. Configure an identity provider (Google, GitHub, etc.) and an access policy (e.g. "allow your email only")

No code changes needed - Cloudflare enforces auth at the edge before traffic reaches your server.

---

## MJPEG over HTTPS

MJPEG streaming works correctly through Cloudflare Tunnel. The tunnel proxies HTTP/1.1 with chunked transfer encoding, which is what MJPEG uses. The `X-Accel-Buffering: no` header the server sets on the `/api/events` SSE endpoint also prevents response buffering on the tunnel side.

If you use a different reverse proxy (nginx, Caddy), ensure it passes through:

```
proxy_buffering off;          # nginx
proxy_http_version 1.1;
proxy_set_header Connection "";
```
