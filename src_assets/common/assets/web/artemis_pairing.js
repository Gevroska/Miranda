import { qrcode } from 'qrcode-generator'

function normalizeArtemisHost(host) {
  const ipv6 = /^\[[a-f0-9:.]+\]$/i.test(host)
  if (!ipv6) {
    const name = host.endsWith('.') ? host.slice(0, -1) : host
    if (name.length > 253 || !name.split('.').every(label => /^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/i.test(label))) {
      return ''
    }
  }

  try {
    const normalized = new URL(`https://${host}`).hostname
    // Reject alternate numeric IPv4 spellings and unspecified addresses.
    if ((!ipv6 && normalized !== host.toLowerCase()) || ['0.0.0.0', '[::]', '[::ffff:0:0]'].includes(normalized)) {
      return ''
    }
    return normalized
  } catch {
    return ''
  }
}

export function defaultArtemisHost(host) {
  const normalized = normalizeArtemisHost(host)
  const loopbackIpv4 = /^127(?:\.[0-9]{1,3}){3}$/.test(normalized)
  if (loopbackIpv4 || /(^|\.)localhost\.?$|^\[::(?:1\]|ffff:7f[0-9a-f]{2}:)/.test(normalized)) {
    return ''
  }
  return normalized
}

function validPort(port) {
  return Number.isInteger(port) && port >= 1 && port <= 65535
}

export function artemisPairingLink(host, portOverride, mappedPort, {pin, passphrase}) {
  const hostname = normalizeArtemisHost(host)
  if (portOverride && !/^[0-9]{1,5}$/.test(portOverride)) {
    return ''
  }
  const port = portOverride ? Number(portOverride) : mappedPort
  if (!hostname || !validPort(port)) {
    return ''
  }

  const link = new URL(`art://${hostname}:${port}`)
  link.searchParams.set('pin', pin)
  link.searchParams.set('passphrase', passphrase)
  return link.href
}

export function artemisQrDataUrl(link) {
  const qr = qrcode(0, 'M')
  qr.addData(link, 'Byte')
  qr.make()
  const moduleSize = 4
  return qr.createDataURL(moduleSize, 4 * moduleSize)
}

export async function mutateArtemisInvitation(method, payload, signal) {
  // Handle failures locally. The shared API helper can log raw JSON parse errors.
  const options = {cache: 'no-store', credentials: 'same-origin', redirect: 'error', signal}
  const tokenResponse = await fetch('./api/csrf-token', {...options, method: 'GET'})
  if (!tokenResponse.ok) {
    throw new Error('Artemis pairing could not acquire a CSRF token')
  }
  const {csrf_token: token} = await tokenResponse.json()
  if (typeof token !== 'string' || !token) {
    throw new Error('Artemis pairing could not acquire a CSRF token')
  }
  signal.throwIfAborted()

  // The server mints the invitation after the request starts. Counting from
  // here avoids extending the displayed lifetime by transit or JSON parsing.
  const startedAt = {wall: Date.now(), steady: performance.now()}
  const response = await fetch('./api/otp', {
    ...options,
    method,
    headers: {'Content-Type': 'application/json', 'X-CSRF-Token': token},
    body: JSON.stringify(payload),
  })
  return {response, startedAt}
}

export function parseArtemisInvitation(result, startedAt) {
  const {status, pin, passphrase, generation, expires_in, game_http_port} = result ?? {}
  if (status !== true || typeof pin !== 'string' || !/^[0-9]{4}$/.test(pin) ||
      typeof passphrase !== 'string' || !/^[a-z0-9._~-]{4,128}$/i.test(passphrase) ||
      typeof generation !== 'string' || !generation ||
      !Number.isSafeInteger(expires_in) || expires_in <= 0 || !validPort(game_http_port)) {
    return null
  }

  const duration = expires_in * 1000
  return {
    generation,
    port: game_http_port,
    credentials: {pin, passphrase},
    wallDeadline: startedAt.wall + duration,
    steadyDeadline: startedAt.steady + duration,
  }
}
