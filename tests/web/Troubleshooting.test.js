import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'

vi.mock('../../src_assets/common/assets/web/Navbar.vue', () => ({
  default: { template: '<div />' },
}))

import Troubleshooting from '../../src_assets/common/assets/web/Troubleshooting.vue'
import createSunshineI18n from '../../src_assets/common/assets/web/locale.js'
import en from '../../src_assets/common/assets/web/public/assets/locale/en.json'

/**
 * @brief Mount troubleshooting with mocked API responses and an optional Vue I18n instance.
 *
 * @param {string} platform Host platform.
 * @param {string} gamepadDriver Selected gamepad driver.
 * @param {object} licenseStatus License status overrides.
 * @param {Array} permissions Platform permissions.
 * @param {object|null} i18n Optional Vue I18n plugin.
 * @param {Array} clients Authoritative paired device state.
 * @return {Promise<object>} Mounted troubleshooting wrapper.
 */
async function mountTroubleshooting(platform, gamepadDriver, licenseStatus = {}, permissions = [], i18n = null, clients = []) {
  vi.stubGlobal('fetch', vi.fn(async (url) => {
    if (url === '/api/config') {
      return { json: async () => ({ platform, gamepad_driver: gamepadDriver }) }
    }
    if (url === '/api/virtual-input/status') {
      return {
        json: async () => ({
          virtualhid: {
            installed: true,
            version: '2026.914.1218',
            version_compatible: true,
            supported_versions: '>= 2026.914.1218',
          },
          vigembus: { installed: false },
        }),
      }
    }
    if (url === '/api/virtual-input/license') {
      return { ok: true, json: async () => ({ service_available: true, state: 'licensed', licensed: true, ...licenseStatus }) }
    }
    if (url === './api/permissions') {
      return { ok: true, json: async () => ({ permissions }) }
    }
    if (url === './api/permissions/request') {
      return { ok: true, json: async () => ({ status: true }) }
    }
    if (url === './api/logs') {
      return { text: async () => '' }
    }
    if (url === './api/clients/list') {
      return { ok: true, json: async () => ({ status: true, named_certs: clients }) }
    }
    return {
      ok: true,
      json: async () => ({ tag_name: 'v2026.914.1218.10', html_url: 'https://example.com/release' }),
    }
  }))

  const wrapper = mount(Troubleshooting, {
    global: {
      mocks: i18n ? {} : { $t: key => key },
      plugins: i18n ? [i18n] : [],
      stubs: { Navbar: true, RouterLink: true },
    },
  })
  await flushPromises()
  return wrapper
}

afterEach(() => {
  vi.unstubAllGlobals()
  vi.restoreAllMocks()
})

describe('virtual input troubleshooting', () => {
  it.each([
    ['valid', false],
    ['malformed', true],
  ])('renders Linux permissions and device management with %s translation messages', async (_description, malformed) => {
    const catalog = structuredClone(en)
    catalog._common.loading = malformed ? 'Loadingchar@@0' : 'Translated loading'
    catalog.troubleshooting.driver_release_checking = malformed ? 'Checkingchar@@0' : 'Translated checking'
    catalog.troubleshooting.permissions_name = malformed ? 'Permissionchar@@0' : 'Translated permission'
    catalog.apps.delete_confirm = 'Delete translated {name}?'
    catalog.troubleshooting.troubleshooting = 'Translated troubleshooting'
    catalog.troubleshooting.unpair_title = 'Translated unpair'
    catalog.troubleshooting.logs = 'Translated logs'

    vi.spyOn(console, 'error').mockImplementation(() => {})
    vi.stubGlobal('fetch', vi.fn(async url => ({
      json: async () => url === './api/configLocale' ? { locale: 'de' } : catalog,
    })))
    const i18n = await createSunshineI18n()
    const wrapper = await mountTroubleshooting('linux', 'none', {}, [
      { id: 'input', status: 'granted', required: true },
    ], i18n)

    try {
      expect(i18n.global.t('_common.loading')).toBe(malformed ? en._common.loading : catalog._common.loading)
      expect(i18n.global.t('troubleshooting.driver_release_checking')).toBe(
        malformed ? en.troubleshooting.driver_release_checking : catalog.troubleshooting.driver_release_checking
      )
      expect(i18n.global.t('apps.delete_confirm', { name: 'Moonlight' })).toBe(
        catalog.apps.delete_confirm.replace('{name}', 'Moonlight')
      )
      expect(wrapper.get('h1').text()).toBe(catalog.troubleshooting.troubleshooting)
      expect(wrapper.get('.permission-table-shell th').text()).toBe(
        malformed ? en.troubleshooting.permissions_name : catalog.troubleshooting.permissions_name
      )
      expect(wrapper.get('#unpair').text()).toBe(catalog.troubleshooting.unpair_title)
      expect(wrapper.get('#logs').text()).toBe(catalog.troubleshooting.logs)
      expect(wrapper.find('.permission-table-shell tbody tr').exists()).toBe(true)
    } finally {
      wrapper.unmount()
      i18n.dispose()
    }
  })

  it('orders permission rows consistently across platforms', async () => {
    const permissions = [
      { id: 'local_network', status: 'on_use', required: true },
      { id: 'config_directory', status: 'granted', required: true },
      { id: 'microphone', status: 'granted', required: false },
      { id: 'system_audio', status: 'on_use', required: true },
      { id: 'notifications', status: 'granted', required: false },
      { id: 'input', status: 'granted', required: true },
      { id: 'screen_recording', status: 'granted', required: true },
    ]
    for (const platform of ['macos', 'windows']) {
      const wrapper = await mountTroubleshooting(platform, 'none', {}, permissions)
      expect(wrapper.findAll('.permission-table-shell tbody tr').map(row => row.get('strong').text())).toEqual([
        'troubleshooting.permission_screen_recording',
        'troubleshooting.permission_input',
        'troubleshooting.permission_notifications',
        'troubleshooting.permission_microphone',
        'troubleshooting.permission_system_audio',
        'troubleshooting.permission_local_network',
        'troubleshooting.permission_config_directory',
      ])
      wrapper.unmount()
    }
  })

  it('shows macOS permission status and requests access from the row button', async () => {
    const wrapper = await mountTroubleshooting('macos', 'all', {}, [
      { id: 'screen_recording', status: 'denied', required: true, requestable: true },
      { id: 'notifications', status: 'granted', required: false },
      { id: 'system_audio', status: 'on_use', required: true, requestable: true },
    ])

    expect(wrapper.get('#permissions').text()).toBe('troubleshooting.permissions_title')
    const card = wrapper.findAll('.card').find(item => item.find('#permissions').exists())
    expect(card.findAll('.permission-table-shell tbody tr')).toHaveLength(3)
    expect(card.get('.permission-table-shell thead').text()).toContain('troubleshooting.permissions_requirement')
    expect(card.findAll('.permission-table-shell tbody button')).toHaveLength(2)
    const screenRow = card.findAll('.permission-table-shell tbody tr').find(row => row.text().includes('permission_screen_recording'))
    expect(screenRow.get('.status-icon').classes()).toContain('status-icon-danger')
    expect(screenRow.get('.status-icon svg').exists()).toBe(true)
    expect(screenRow.get('.status-icon .visually-hidden').text()).toBe('troubleshooting.permissions_status_denied')
    expect(screenRow.get('.status-icon').attributes('title')).toBe('troubleshooting.permissions_status_denied')
    expect(screenRow.get('.status-icon').attributes('role')).toBeUndefined()
    expect(screenRow.get('.status-icon').attributes('tabindex')).toBeUndefined()
    const audioRow = card.findAll('.permission-table-shell tbody tr').find(row => row.text().includes('permission_system_audio'))
    expect(audioRow.get('.status-icon').classes()).toContain('status-icon-primary')
    expect(audioRow.get('.status-icon').attributes('title')).toBe('troubleshooting.permissions_status_on_use')
    await screenRow.get('button').trigger('click')
    await flushPromises()
    expect(fetch).toHaveBeenCalledWith('./api/permissions/request', expect.objectContaining({
      method: 'POST',
      body: JSON.stringify({ id: 'screen_recording' }),
    }))
    expect(screenRow.text()).toContain('troubleshooting.permission_screen_recording_help')
    wrapper.unmount()
  })

  it('shows macOS input access recovery steps when the request button is used', async () => {
    const wrapper = await mountTroubleshooting('macos', 'all', {}, [
      { id: 'input', status: 'denied', required: true, requestable: true },
    ])

    const row = wrapper.get('.permission-table-shell tbody tr')
    await row.get('button').trigger('click')
    await flushPromises()
    expect(fetch).toHaveBeenCalledWith('./api/permissions/request', expect.objectContaining({
      method: 'POST',
      body: JSON.stringify({ id: 'input' }),
    }))
    expect(row.text()).toContain('troubleshooting.permission_input_help')
    wrapper.unmount()
  })

  it('shows Local Network setup steps after opening Privacy & Security', async () => {
    const wrapper = await mountTroubleshooting('macos', 'all', {}, [
      { id: 'local_network', status: 'on_use', required: true, requestable: true },
    ])

    const row = wrapper.get('.permission-table-shell tbody tr')
    expect(row.get('button').text()).toBe('troubleshooting.permissions_privacy_settings')
    await row.get('button').trigger('click')
    await flushPromises()
    expect(fetch).toHaveBeenCalledWith('./api/permissions/request', expect.objectContaining({
      method: 'POST',
      body: JSON.stringify({ id: 'local_network' }),
    }))
    expect(row.text()).toContain('troubleshooting.permission_local_network_help')
    wrapper.unmount()
  })

  it('shows Windows directory access with setup steps', async () => {
    const wrapper = await mountTroubleshooting('windows', 'all', {}, [
      { id: 'config_directory', status: 'denied', required: true, verifiable: true, requestable: false },
    ])
    expect(wrapper.find('#permissions').exists()).toBe(true)
    const row = wrapper.get('.permission-table-shell tbody tr')
    await row.get('button').trigger('click')
    expect(row.find('output').exists()).toBe(true)
    expect(row.text()).toContain('troubleshooting.permission_config_directory_help')
    expect(fetch).not.toHaveBeenCalledWith('./api/permissions/request', expect.anything())
    wrapper.unmount()
  })

  it('shows Linux input setup steps', async () => {
    const wrapper = await mountTroubleshooting('linux', 'none', {}, [
      { id: 'input', status: 'denied', required: true, verifiable: true, requestable: false },
    ])
    await wrapper.get('.permission-table-shell tbody button').trigger('click')
    expect(wrapper.text()).toContain('troubleshooting.permission_input_help_linux')
    wrapper.unmount()
  })

  it('shows the broker version table without ViGEmBus on macOS', async () => {
    const wrapper = await mountTroubleshooting('macos', 'all')

    expect(wrapper.find('.virtual-gamepad-section').exists()).toBe(true)
    expect(wrapper.findAll('.driver-table-shell tbody tr')).toHaveLength(1)
    expect(wrapper.find('.driver-table-shell').text()).toContain('2026.914.1218')
    const status = wrapper.get('.driver-status-icon')
    expect(status.classes()).toContain('status-icon-success')
    expect(status.get('svg').exists()).toBe(true)
    expect(status.get('.visually-hidden').text()).toBe('troubleshooting.driver_status_compatible')
    expect(status.attributes('title')).toBe('troubleshooting.driver_status_compatible')
    expect(status.attributes('role')).toBeUndefined()
    expect(status.attributes('tabindex')).toBeUndefined()
    expect(fetch).toHaveBeenCalledWith('/api/virtual-input/status')
    wrapper.unmount()
  })

  it.each(['macos', 'windows'])('shows only the license activation limit on %s', async platform => {
    const wrapper = await mountTroubleshooting(platform, 'all', { activation_usage: 1, activation_limit: 5 })

    const activation = wrapper.findAll('.virtualhid-license-stat').find(stat =>
      stat.find('dt').text() === 'troubleshooting.virtualhid_license_activation_limit'
    )
    expect(activation.find('dd').text()).toBe('5')
    wrapper.unmount()
  })

  it('keeps both backend rows on Windows when all backends are enabled', async () => {
    const wrapper = await mountTroubleshooting('windows', 'all')

    expect(wrapper.findAll('.driver-table-shell tbody tr')).toHaveLength(2)
    const vigembusStatus = wrapper.get('#vigembus .driver-status-icon')
    expect(vigembusStatus.classes()).toContain('status-icon-neutral')
    expect(vigembusStatus.attributes('title')).toBe('troubleshooting.driver_status_not_installed')
    expect(vigembusStatus.get('.visually-hidden').text()).toBe('troubleshooting.driver_status_not_installed')
    wrapper.vm.virtualhid.version_compatible = false
    await wrapper.vm.$nextTick()
    const unsupportedStatus = wrapper.get('.driver-table-shell tbody tr:first-child .driver-status-icon')
    expect(unsupportedStatus.classes()).toContain('status-icon-danger')
    expect(unsupportedStatus.attributes('title')).toBe('troubleshooting.driver_status_unsupported')
    expect(unsupportedStatus.get('.visually-hidden').text()).toBe('troubleshooting.driver_status_unsupported')
    wrapper.unmount()
  })

  it('shows distinct icons for granted, unrequested, and unknown permissions', async () => {
    const wrapper = await mountTroubleshooting('macos', 'all', {}, [
      { id: 'notifications', status: 'granted', required: false },
      { id: 'microphone', status: 'not_determined', required: false, requestable: true },
      { id: 'input', status: 'unknown', required: true },
    ])

    const icons = wrapper.findAll('.permission-table-shell tbody .status-icon')
    expect(icons.every(icon => icon.find('svg').exists())).toBe(true)
    expect(icons.map(icon => icon.classes().find(name => name.startsWith('status-icon-')))).toEqual([
      'status-icon-warning', 'status-icon-success', 'status-icon-neutral',
    ])
    expect(icons.map(icon => icon.attributes('title'))).toEqual([
      'troubleshooting.permissions_status_unknown',
      'troubleshooting.permissions_status_granted',
      'troubleshooting.permissions_status_not_determined',
    ])
    expect(wrapper.findAll('.permission-table-shell tbody button')).toHaveLength(2)
    wrapper.unmount()
  })
})

describe('paired device rights', () => {
  it.each(['success', 'lost response'])('saves display preferences with authoritative readback after %s without changing device grants', async outcome => {
    const clients = [{ uuid: 'paired-device', name: 'Paired device', enabled: false, permissions: 0, display_valid: true,
      display: { force_virtual: false, resolution: null, refresh_rate: null } }]
    const wrapper = await mountTroubleshooting('windows', 'none', {}, [], null, clients)
    const originalFetch = fetch
    vi.stubGlobal('fetch', vi.fn(async (url, options) => {
      if (url === './api/clients/update') {
        const body = JSON.parse(options.body)
        expect(body).toEqual({ uuid: 'paired-device', display: { force_virtual: true, resolution: null, refresh_rate: { numerator: 60000, denominator: 1001 } } })
        clients[0].display = { ...body.display }
        if (outcome === 'lost response') throw new TypeError('The committed response was lost')
        return { ok: true, json: async () => ({ status: true }) }
      }
      return originalFetch(url, options)
    }))
    await wrapper.get('[data-display-force]').setValue(true)
    await wrapper.get('[data-display-rate]').setValue('60000/1001')
    await wrapper.get('[data-display-rate]').element.closest('fieldset').querySelector('button').click()
    await flushPromises()
    expect(wrapper.get('[data-display-force]').element.checked).toBe(true)
    expect(wrapper.get('#toggle-paired-device').element.checked).toBe(false)
    expect(wrapper.findAll('[data-device-right]').every(input => input.element.checked === false)).toBe(true)
    expect(wrapper.vm.clientUpdateError).toBe('')
    wrapper.unmount()
  })

  it('keeps device display preferences absent from non-Windows administration', async () => {
    const wrapper = await mountTroubleshooting('linux', 'none', {}, [], null, [
      { uuid: 'paired-device', name: 'Paired device', enabled: true, permissions: 0 },
    ])
    expect(wrapper.find('[data-display-force]').exists()).toBe(false)
    wrapper.unmount()
  })

  it('shows only implemented explicit grants and saves rights without changing the enabled gate', async () => {
    const clients = [{ uuid: 'paired-device', name: 'Paired device', enabled: false, permissions: 0 }]
    const wrapper = await mountTroubleshooting('linux', 'none', {}, [], null, clients)
    const rights = wrapper.findAll('[data-device-right]')
    expect(rights).toHaveLength(8)
    expect(rights.every(input => input.element.checked === false)).toBe(true)
    expect(wrapper.text()).not.toContain('device_right_clipboard')
    const originalFetch = fetch
    vi.stubGlobal('fetch', vi.fn(async (url, options) => {
      if (url === './api/clients/update') {
        const body = JSON.parse(options.body)
        expect(body).toEqual({ uuid: 'paired-device', permissions: 0x02000000 })
        clients[0].permissions = body.permissions
        return { ok: true, json: async () => ({ status: true }) }
      }
      return originalFetch(url, options)
    }))
    await wrapper.get('[data-device-right="33554432"]').setValue(true)
    await wrapper.get('fieldset button').trigger('click')
    await flushPromises()
    expect(wrapper.get('#toggle-paired-device').element.checked).toBe(false)
    expect(wrapper.get('[data-device-right="33554432"]').element.checked).toBe(true)
    expect(wrapper.vm.clientUpdateError).toBe('')
    wrapper.unmount()
  })

  it('reports rejected updates and restores the authoritative saved rights', async () => {
    const wrapper = await mountTroubleshooting('linux', 'none', {}, [], null, [
      { uuid: 'paired-device', name: 'Paired device', enabled: true, permissions: 0 },
    ])
    const originalFetch = fetch
    vi.stubGlobal('fetch', vi.fn(async (url, options) => {
      if (url === './api/clients/update') { return { ok: true, json: async () => ({ status: false }) } }
      return originalFetch(url, options)
    }))
    await wrapper.get('[data-device-right="16777216"]').setValue(true)
    await wrapper.get('fieldset button').trigger('click')
    await flushPromises()
    expect(wrapper.get('[role="alert"]').text()).toBe('troubleshooting.device_update_error')
    expect(wrapper.get('[data-device-right="16777216"]').element.checked).toBe(false)
    expect(wrapper.get('#toggle-paired-device').element.checked).toBe(true)
    wrapper.unmount()
  })

  it.each(['transport', 'JSON'])('confirms saved rights after the server commits but its %s response is lost', async failure => {
    const clients = [{ uuid: 'paired-device', name: 'Paired device', enabled: false, permissions: 0 }]
    const wrapper = await mountTroubleshooting('linux', 'none', {}, [], null, clients)
    const originalFetch = fetch
    vi.stubGlobal('fetch', vi.fn(async (url, options) => {
      if (url === './api/clients/update') {
        clients[0].permissions = JSON.parse(options.body).permissions
        if (failure === 'transport') throw new TypeError('The committed response was lost')
        return { ok: true, json: async () => { throw new SyntaxError('Truncated committed response') } }
      }
      return originalFetch(url, options)
    }))
    await wrapper.get('[data-device-right="33554432"]').setValue(true)
    await wrapper.get('fieldset button').trigger('click')
    await flushPromises()
    expect(wrapper.get('[data-device-right="33554432"]').element.checked).toBe(true)
    expect(wrapper.get('#toggle-paired-device').element.checked).toBe(false)
    expect(wrapper.vm.clientUpdateError).toBe('')
    expect(wrapper.vm.clientUpdatePending).toBe(null)
    wrapper.unmount()
  })

  it.each([
    ['HTTP failure', () => ({ ok: false, json: async () => ({ status: true, named_certs: [] }) })],
    ['rejected readback', () => ({ ok: true, json: async () => ({ status: false, named_certs: [] }) })],
    ['malformed readback', () => ({ ok: true, json: async () => ({ status: true, named_certs: [{}] }) })],
    ['unreadable JSON', () => ({ ok: true, json: async () => { throw new SyntaxError('Truncated response') } })],
  ])('reports uncertainty when a committed response is lost and authoritative refresh has %s', async (_reason, readback) => {
    const clients = [{ uuid: 'paired-device', name: 'Paired device', enabled: true, permissions: 0 }]
    const wrapper = await mountTroubleshooting('linux', 'none', {}, [], null, clients)
    const originalFetch = fetch
    vi.stubGlobal('fetch', vi.fn(async (url, options) => {
      if (url === './api/clients/update') {
        clients[0].permissions = JSON.parse(options.body).permissions
        throw new TypeError('The committed response was lost')
      }
      if (url === './api/clients/list') return readback()
      return originalFetch(url, options)
    }))
    await wrapper.get('[data-device-right="16777216"]').setValue(true)
    await wrapper.get('fieldset button').trigger('click')
    await flushPromises()
    expect(wrapper.get('[role="alert"]').text()).toBe('troubleshooting.device_update_unknown')
    expect(wrapper.vm.clientUpdateError).not.toBe('troubleshooting.device_update_error')
    expect(wrapper.vm.clientUpdatePending).toBe(null)
    wrapper.unmount()
  })

  it('keeps the mutation pending until its authoritative readback completes', async () => {
    const clients = [{ uuid: 'paired-device', name: 'Paired device', enabled: true, permissions: 0 }]
    const wrapper = await mountTroubleshooting('linux', 'none', {}, [], null, clients)
    const originalFetch = fetch
    let finishReadback
    vi.stubGlobal('fetch', vi.fn(async (url, options) => {
      if (url === './api/clients/update') {
        clients[0].permissions = JSON.parse(options.body).permissions
        return { ok: true, json: async () => ({ status: true }) }
      }
      if (url === './api/clients/list') return new Promise(resolve => { finishReadback = resolve })
      return originalFetch(url, options)
    }))
    const operation = wrapper.vm.updateClient('paired-device', { permissions: 0x01000000 })
    await flushPromises()
    expect(wrapper.vm.clientUpdatePending).toBe('paired-device')
    expect(finishReadback).toBeTypeOf('function')
    finishReadback({ ok: true, json: async () => ({ status: true, named_certs: clients }) })
    await operation
    await flushPromises()
    expect(wrapper.vm.clientUpdatePending).toBe(null)
    expect(wrapper.get('[data-device-right="16777216"]').element.checked).toBe(true)
    wrapper.unmount()
  })

  it.each([true, false])('handles a committed unpair with a lost response and readback success=%s', async readbackSucceeds => {
    const clients = [{ uuid: 'paired-device', name: 'Paired device', enabled: true, permissions: 0 }]
    const wrapper = await mountTroubleshooting('linux', 'none', {}, [], null, clients)
    const originalFetch = fetch
    vi.stubGlobal('fetch', vi.fn(async (url, options) => {
      if (url === './api/clients/unpair') {
        clients.splice(0)
        throw new TypeError('The committed response was lost')
      }
      if (url === './api/clients/list' && !readbackSucceeds) throw new TypeError('Readback unavailable')
      return originalFetch(url, options)
    }))
    await wrapper.vm.unpairSingle('paired-device')
    await flushPromises()
    expect(wrapper.vm.clientUpdatePending).toBe(null)
    if (readbackSucceeds) {
      expect(wrapper.vm.clients).toEqual([])
      expect(wrapper.vm.clientUpdateError).toBe('')
    } else {
      expect(wrapper.vm.clients).toHaveLength(1)
      expect(wrapper.get('[role="alert"]').text()).toBe('troubleshooting.device_update_unknown')
    }
    wrapper.unmount()
  })
})
