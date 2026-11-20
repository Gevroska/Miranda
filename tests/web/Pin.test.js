// @vitest-environment-options {"url":"https://proxy.example.test:8443/pin"}

import { flushPromises, mount } from '@vue/test-utils'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { qrcode } from 'qrcode-generator'

import Pin from '../../src_assets/common/assets/web/Pin.vue'
import { artemisPairingLink, defaultArtemisHost } from '../../src_assets/common/assets/web/artemis_pairing'

vi.mock('../../src_assets/common/assets/web/Navbar.vue', () => ({default: {template: '<nav />'}}))

vi.mock('qrcode-generator', async importOriginal => {
  const actual = await importOriginal()
  return {qrcode: vi.fn(actual.qrcode)}
})

const invitation = {
  status: true,
  pin: '0042',
  passphrase: 'dummy-passphrase',
  generation: 'dummy-generation',
  expires_in: 180,
  game_http_port: 47989,
}

const response = (body, status = 200) => new Response(JSON.stringify(body), {status})

describe('Artemis pairing authority', () => {
  it.each([
    ['Home-Host.example.', '', 47989, 'home-host.example.:47989'],
    ['192.0.2.6', '', 48389, '192.0.2.6:48389'],
    ['[2001:0DB8::4]', '9111', 47989, '[2001:db8::4]:9111'],
    ['miranda', '65535', 47989, 'miranda:65535'],
    ['device.local', '1', 47989, 'device.local:1'],
  ])('builds an explicit authority for %s and port %s', (host, override, mapped, authority) => {
    expect(artemisPairingLink(host, override, mapped, invitation))
      .toBe(`art://${authority}?pin=0042&passphrase=dummy-passphrase`)
  })

  it.each([
    '', 'user@host', 'host:1234', 'host/path', 'host\\path', 'host?pin=42', 'host#fragment',
    'host%2fpath', 'host\tname', 'host\n', '*.example.test', '-host', 'host-.test', 'host..test',
    '192.0.2.999', '192.000.2.6', '127.1', '0x7f000001', '0.0.0.0', '2001:db8::4', '[::]', '[::ffff:0.0.0.0]', '[::zzz]', '[fe80::5%42]',
  ])('rejects invalid authority %j', host => {
    expect(artemisPairingLink(host, '', 47989, invitation)).toBe('')
  })

  it.each(['0', '65536', '1e3', '22.1', '+4', ' 123', '22/'])('rejects invalid port %j', port => {
    expect(artemisPairingLink('host', port, 47989, invitation)).toBe('')
  })

  it.each(['localhost', 'LOCALHOST.', 'child.localhost', '127.30.0.1', '[::1]', '[::ffff:127.0.0.1]'])('does not propose loopback %s', host => {
    expect(defaultArtemisHost(host)).toBe('')
  })

  it.each(['127.home.arpa', '127.0.0.1.example.test'])('proposes DNS names that start with 127, including %s', host => {
    expect(defaultArtemisHost(host)).toBe(host)
  })
})

describe('PIN pairing', () => {
  let wrapper
  let fetchMock
  let generate
  let withdraw
  let tokensIssued
  let postOtp
  let deleteOtp
  let loadToken

  const calls = url => fetchMock.mock.calls.filter(([path]) => path === url).map(([, options]) => options)

  function mountPage() {
    wrapper = mount(Pin, {
      global: {
        mocks: {$t: (key, values) => values ? `${key} ${values.seconds}` : key},
        provide: {i18n: {t: key => key}},
      },
    })
    const button = (label) => wrapper.findAll('button').find(button => button.text() === label)
    generate = button('pin.artemis_generate')
    withdraw = button('pin.artemis_withdraw')
  }

  function expectNoCredentials() {
    expect(wrapper.find('#artemis-link').exists()).toBe(false)
    expect(wrapper.find('img').exists()).toBe(false)
    expect(wrapper.find('#artemis-pin').exists()).toBe(false)
    expect(wrapper.find('#artemis-passphrase').exists()).toBe(false)
  }

  beforeEach(() => {
    vi.useFakeTimers({toFake: ['setTimeout', 'clearTimeout', 'setInterval', 'clearInterval', 'Date', 'performance']})
    tokensIssued = 0
    loadToken = () => response({csrf_token: `dummy-token-${++tokensIssued}`})
    postOtp = () => response(invitation)
    deleteOtp = () => response({status: true})
    fetchMock = vi.fn(async (url, options) => {
      if (url === './api/csrf-token') {
        return loadToken()
      }
      if (url === './api/otp') {
        return options.method === 'POST' ? postOtp() : deleteOtp()
      }
      if (options.method === 'GET') {
        return response({pairings: [{id: 'pending-device', name: 'New Device', address: '192.0.2.6'}]})
      }
      return response({status: true})
    })
    vi.stubGlobal('fetch', fetchMock)
    mountPage()
  })

  afterEach(() => {
    wrapper?.unmount()
    wrapper = null
    vi.useRealTimers()
    vi.unstubAllGlobals()
    vi.restoreAllMocks()
  })

  it('keeps manual pending approvals and cancellation available without requesting an invitation on mount', async () => {
    await flushPromises()
    expect(calls('./api/otp')).toHaveLength(0)
    expect(calls('./api/csrf-token')).toHaveLength(0)
    expectNoCredentials()
    expect(withdraw.element.disabled).toBe(true)
    expect(wrapper.get('#pairing-input').element.value).toBe('pending-device')

    await wrapper.get('#pin-input').setValue('0007')
    await wrapper.get('#name-input').setValue('Guest')
    await wrapper.get('#form').trigger('submit')
    await flushPromises()
    expect(JSON.parse(calls('./api/pin').find(call => call.method === 'POST').body))
      .toEqual({pairing_id: 'pending-device', pin: '0007', name: 'Guest'})
    expect(wrapper.get('#pin-input').element.value).toBe('')
    expect(wrapper.get('#name-input').element.value).toBe('')

    await wrapper.get('#form button[type="button"]').trigger('click')
    expect(JSON.parse(calls('./api/pin').find(call => call.method === 'DELETE').body))
      .toEqual({pairing_id: 'pending-device'})
  })

  it('shows the typed values and encodes the identical link locally using the mapped port behind a proxy', async () => {
    postOtp = () => response({...invitation, game_http_port: 55000, name: 'ignore&pin=5678'})
    await generate.trigger('click')
    await flushPromises()

    expect(wrapper.get('#artemis-host').element.value).toBe('proxy.example.test')
    expect(wrapper.get('#artemis-port').attributes('placeholder')).toBe('55000')
    expect(wrapper.get('#artemis-pin').text()).toBe('0042')
    expect(wrapper.get('#artemis-passphrase').text()).toBe('dummy-passphrase')
    const href = wrapper.get('#artemis-link').attributes('href')
    expect(href).toBe('art://proxy.example.test:55000?pin=0042&passphrase=dummy-passphrase')
    const {qrcode: realQr} = await vi.importActual('qrcode-generator')
    const expectedQr = realQr(0, 'M')
    expectedQr.addData(href, 'Byte')
    expectedQr.make()
    expect(wrapper.get('img').attributes('src')).toMatch(/^data:image\/gif;base64,/)
    expect(wrapper.get('img').attributes('src')).toBe(expectedQr.createDataURL(4, 16))

    await wrapper.get('#artemis-host').setValue('[2001:db8::10]')
    await wrapper.get('#artemis-port').setValue('61423')
    expect(wrapper.get('#artemis-link').attributes('href'))
      .toBe('art://[2001:db8::10]:61423?pin=0042&passphrase=dummy-passphrase')
    expect(calls('./api/otp')).toHaveLength(1)
  })

  it('allows typed pairing without an address, and removes the QR and link while the address is invalid', async () => {
    const host = wrapper.get('#artemis-host')
    await host.setValue('')
    await generate.trigger('click')
    await flushPromises()
    expect(wrapper.get('#artemis-pin').text()).toBe('0042')
    expect(wrapper.find('#artemis-link').exists()).toBe(false)
    await host.setValue('192.0.2.6')
    expect(wrapper.get('#artemis-link').attributes('href')).toBe('art://192.0.2.6:47989?pin=0042&passphrase=dummy-passphrase')
    await host.setValue('user@example.test')
    expect(wrapper.find('#artemis-link').exists()).toBe(false)
    expect(wrapper.find('img').exists()).toBe(false)
  })

  it('hides secrets immediately on withdrawal, requests fresh CSRF and blocks overlap', async () => {
    await generate.trigger('click')
    await flushPromises()
    const pending = Promise.withResolvers()
    deleteOtp = () => pending.promise
    await withdraw.trigger('click')
    expectNoCredentials()
    await generate.trigger('click')
    await withdraw.trigger('click')
    await flushPromises()
    expect(calls('./api/csrf-token')).toEqual([
      expect.objectContaining({method: 'GET', cache: 'no-store', redirect: 'error'}),
      expect.objectContaining({method: 'GET', cache: 'no-store', redirect: 'error'}),
    ])
    expect(calls('./api/otp')).toEqual([
      expect.objectContaining({method: 'POST', cache: 'no-store', redirect: 'error', body: '{}', headers: {
        'Content-Type': 'application/json', 'X-CSRF-Token': 'dummy-token-1',
      }}),
      expect.objectContaining({method: 'DELETE', cache: 'no-store', redirect: 'error', body: JSON.stringify({generation: invitation.generation}), headers: {
        'Content-Type': 'application/json', 'X-CSRF-Token': 'dummy-token-2',
      }}),
    ])
    pending.resolve(response({status: true}))
    await flushPromises()
    expectNoCredentials()
    expect(wrapper.get('[role="status"]').text()).toBe('pin.artemis_withdrawn')
    expect(withdraw.element.disabled).toBe(true)
  })

  it('keeps only the current generation after replacing and allows an explicit withdrawal retry after a failure', async () => {
    await generate.trigger('click')
    await flushPromises()
    const replacement = Promise.withResolvers()
    postOtp = () => replacement.promise
    await generate.trigger('click')
    await generate.trigger('click')
    await withdraw.trigger('click')
    expectNoCredentials()
    replacement.resolve(response({...invitation, generation: 'replacement', pin: '0018', passphrase: 'new-dummy-passphrase'}))
    await flushPromises()
    expect(wrapper.get('#artemis-pin').text()).toBe('0018')
    expect(calls('./api/otp')).toHaveLength(2)

    deleteOtp = () => Promise.reject(new Error('dummy-private-response'))
    await withdraw.trigger('click')
    await flushPromises()
    expectNoCredentials()
    expect(wrapper.get('[role="status"]').text()).toBe('pin.artemis_withdraw_failure')
    expect(calls('./api/otp')).toHaveLength(3)

    deleteOtp = () => response({error: 'dummy-private-response'}, 409)
    await withdraw.trigger('click')
    await flushPromises()
    const deletes = calls('./api/otp').filter(call => call.method === 'DELETE')
    expect(deletes.map(call => JSON.parse(call.body))).toEqual([{generation: 'replacement'}, {generation: 'replacement'}])
    expect(deletes.map(call => call.headers['X-CSRF-Token'])).toEqual(['dummy-token-3', 'dummy-token-4'])
    expect(wrapper.get('[role="status"]').text()).toBe('pin.artemis_unavailable')
    expect(withdraw.element.disabled).toBe(true)
  })

  it('keeps the previous withdrawal available when replacement fails without redisplaying its secrets', async () => {
    await generate.trigger('click')
    await flushPromises()
    postOtp = () => response({}, 500)
    await generate.trigger('click')
    await flushPromises()
    expectNoCredentials()
    expect(withdraw.element.disabled).toBe(false)
    await withdraw.trigger('click')
    await flushPromises()
    expect(JSON.parse(calls('./api/otp').at(-1).body)).toEqual({generation: invitation.generation})
    expect(wrapper.get('[role="status"]').text()).toBe('pin.artemis_withdrawn')
  })

  it('counts from the dispatch time and removes locally expired credentials at the deadline', async () => {
    const pending = Promise.withResolvers()
    postOtp = () => pending.promise
    await generate.trigger('click')
    await flushPromises()
    await vi.advanceTimersByTimeAsync(1500)
    pending.resolve(response({...invitation, expires_in: 3}))
    await flushPromises()
    expect(wrapper.text()).toContain('pin.artemis_expires_in 2')
    await vi.advanceTimersByTimeAsync(1499)
    expect(wrapper.text()).toContain('pin.artemis_expires_in 1')
    expect(wrapper.find('#artemis-link').exists()).toBe(true)
    await vi.advanceTimersByTimeAsync(1)
    expectNoCredentials()
    expect(wrapper.get('[role="status"]').text()).toBe('pin.artemis_expired')
  })

  it('clears an invitation when visibility returns after the wall clock passes the deadline', async () => {
    await generate.trigger('click')
    await flushPromises()
    vi.setSystemTime(Date.now() + invitation.expires_in * 1000)
    document.dispatchEvent(new Event('visibilitychange'))
    await flushPromises()
    expectNoCredentials()
  })

  it('blocks an expired link when the timer has not had a chance to run', async () => {
    await generate.trigger('click')
    await flushPromises()
    const link = wrapper.get('#artemis-link').element
    vi.setSystemTime(Date.now() + invitation.expires_in * 1000)
    expect(link.dispatchEvent(new MouseEvent('click', {bubbles: true, cancelable: true}))).toBe(false)
    await flushPromises()
    expectNoCredentials()
  })

  it('discards responses delivered after their full lifetime', async () => {
    const pending = Promise.withResolvers()
    postOtp = () => pending.promise
    await generate.trigger('click')
    await flushPromises()
    await vi.advanceTimersByTimeAsync(1000)
    pending.resolve(response({...invitation, expires_in: 1}))
    await flushPromises()
    expectNoCredentials()
    expect(withdraw.element.disabled).toBe(true)
    expect(wrapper.get('[role="status"]').text()).toBe('pin.artemis_expired')
  })

  it.each([
    ['expiry', page => {
      vi.setSystemTime(Date.now() + invitation.expires_in * 1000)
      page.vm.updateArtemisCountdown()
    }],
    ['withdrawal', page => page.vm.withdrawArtemisInvitation()],
    ['replacement', page => page.vm.generateArtemisInvitation()],
    ['unmount', page => {
      page.unmount()
      wrapper = null
    }],
  ])('clears the stored display state synchronously on %s', async (_reason, clear) => {
    await generate.trigger('click')
    await flushPromises()
    const state = wrapper.vm.$data
    expect(state.artemisLink).toContain('pin=0042')
    expect(state.artemisQrImage).toMatch(/^data:image\/gif;base64,/)
    expect(state.artemisInvitation.credentials).toEqual({pin: '0042', passphrase: 'dummy-passphrase'})

    const pending = clear(wrapper)
    expect(state.artemisLink).toBe('')
    expect(state.artemisQrImage).toBe('')
    expect(state.artemisInvitation?.credentials ?? null).toBeNull()
    await pending
  })

  it('drops a previous QR when replacement fails to encode and handles failed image decoding', async () => {
    const error = vi.spyOn(console, 'error').mockImplementation(() => {})
    await generate.trigger('click')
    await flushPromises()
    expect(wrapper.find('img').exists()).toBe(true)
    vi.mocked(qrcode).mockImplementationOnce(() => { throw new Error('dummy-private-response') })
    postOtp = () => response({...invitation, pin: '0009'})
    await generate.trigger('click')
    await flushPromises()
    expect(wrapper.get('#artemis-pin').text()).toBe('0009')
    expect(wrapper.get('#artemis-link').attributes('href')).toContain('pin=0009')
    expect(wrapper.find('img').exists()).toBe(false)
    expect(wrapper.vm.$data.artemisQrImage).toBe('')
    expect(wrapper.text()).toContain('pin.artemis_qr_failure')
    expect(error).not.toHaveBeenCalled()

    await wrapper.get('#artemis-host').setValue('another-host.test')
    await wrapper.get('img').trigger('error')
    expect(wrapper.find('img').exists()).toBe(false)
    expect(wrapper.vm.$data.artemisQrImage).toBe('')
    await wrapper.get('#artemis-port').setValue('49876')
    expect(wrapper.find('img').exists()).toBe(true)
  })

  it('shows only safe errors for CSRF failures, rejected responses and invalid payloads', async () => {
    const error = vi.spyOn(console, 'error').mockImplementation(() => {})
    const debug = vi.spyOn(console, 'debug').mockImplementation(() => {})
    const warning = vi.spyOn(console, 'warn').mockImplementation(() => {})
    loadToken = () => response({error: 'dummy-private-response'}, 401)
    await generate.trigger('click')
    await flushPromises()
    expect(calls('./api/otp')).toHaveLength(0)
    expect(wrapper.get('[role="status"]').text()).toBe('pin.artemis_generate_failure')

    loadToken = () => response({csrf_token: 'dummy-token'})
    postOtp = () => new Response('dummy-private-response', {status: 400})
    await generate.trigger('click')
    await flushPromises()
    expect(calls('./api/otp')).toHaveLength(1)
    expect(wrapper.get('[role="status"]').text()).toBe('pin.artemis_generate_failure')

    postOtp = () => response({...invitation, passphrase: 'dummy&pin=unsafe'})
    await generate.trigger('click')
    await flushPromises()
    expect(calls('./api/otp')).toHaveLength(2)
    expect(wrapper.get('[role="status"]').text()).toBe('pin.artemis_generate_failure')
    expectNoCredentials()
    expect(wrapper.html()).not.toContain('dummy-private-response')
    expect(error).not.toHaveBeenCalled()
    expect(debug).not.toHaveBeenCalled()
    expect(warning).not.toHaveBeenCalled()
  })

  it('prevents an outstanding CSRF request from dispatching a mutation after unmount', async () => {
    const pending = Promise.withResolvers()
    loadToken = () => pending.promise
    await generate.trigger('click')
    wrapper.unmount()
    wrapper = null
    pending.resolve(response({csrf_token: 'dummy-token'}))
    await flushPromises()
    expect(calls('./api/csrf-token')[0].signal.aborted).toBe(true)
    expect(calls('./api/otp')).toHaveLength(0)
    expect(vi.getTimerCount()).toBe(0)
  })

  it('clears a page leaving for browser history and ignores its delayed response', async () => {
    const pending = Promise.withResolvers()
    postOtp = () => pending.promise
    await generate.trigger('click')
    await flushPromises()
    window.dispatchEvent(new Event('pagehide'))
    pending.resolve(response(invitation))
    await flushPromises()
    expect(calls('./api/otp')[0].signal.aborted).toBe(true)
    expectNoCredentials()

    postOtp = () => response(invitation)
    await generate.trigger('click')
    await flushPromises()
    expect(wrapper.find('#artemis-link').exists()).toBe(true)
    window.dispatchEvent(new Event('pagehide'))
    await flushPromises()
    expectNoCredentials()
    expect(vi.getTimerCount()).toBe(1)
  })
})
