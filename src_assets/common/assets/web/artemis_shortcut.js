const uuidPattern = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

export function isArtemisUuid(value) {
  return typeof value === 'string' && value.length === 36 && uuidPattern.test(value);
}

function normalizeLabel(value) {
  if (typeof value !== 'string') return '';

  // Unicode mode matches lone surrogates here while preserving valid pairs.
  return value
    .replace(/[\uD800-\uDFFF]/gu, '\uFFFD')
    .replace(/[\p{Cc}\p{Zl}\p{Zp}]+/gu, ' ')
    .trim();
}

export function buildArtemisShortcut(host, app) {
  if (!host || !isArtemisUuid(app.uuid)) return null;

  const fields = [
    ['host_uuid', host.uuid],
    ['host_name', normalizeLabel(host.name)],
    ['app_uuid', app.uuid],
    ['app_name', normalizeLabel(app.name)],
  ].filter(([, value]) => value);

  const query = fields.map(([key, value]) => key + '=' + encodeURIComponent(value)).join('&');
  return {
    uri: 'art://launch?' + query,
    filename: 'Miranda-' + app.uuid + '.art',
    content: fields.map(([key, value]) => '[' + key + '] ' + value + '\n').join(''),
  };
}
