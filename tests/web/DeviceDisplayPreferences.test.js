import { mount } from '@vue/test-utils'
import { describe, expect, it } from 'vitest'

import DeviceDisplayPreferences from '../../src_assets/common/assets/web/DeviceDisplayPreferences.vue'

function mountPreferences(preferences = { force_virtual: false, resolution: null, refresh_rate: null }, props = {}) {
  return mount(DeviceDisplayPreferences, {
    props: { deviceId: 'paired-device', preferences, ...props },
    global: { mocks: { $t: key => key } },
  })
}

describe('device display preferences', () => {
  it.each([
    ['60000/1001', { numerator: 60000, denominator: 1001 }],
    ['59.94', { numerator: 2997, denominator: 50 }],
    ['120000/2002', { numerator: 60000, denominator: 1001 }],
    ['165', { numerator: 165, denominator: 1 }],
  ])('saves exact source hertz from %s without granting device rights', async (text, refresh_rate) => {
    const wrapper = mountPreferences()
    await wrapper.get('[data-display-force]').setValue(true)
    await wrapper.get('[data-display-width]').setValue('2560')
    await wrapper.get('[data-display-height]').setValue('1440')
    await wrapper.get('[data-display-rate]').setValue(text)
    await wrapper.get('button').trigger('click')
    expect(wrapper.emitted('save')).toEqual([[{ force_virtual: true, resolution: { width: 2560, height: 1440 }, refresh_rate }]])
    expect(wrapper.get('[data-display-width]').attributes('id')).toBe('display-width-paired-device')
    expect(wrapper.get('label[for="display-rate-paired-device"]').text()).toBe('display_preferences.refresh_rate')
    wrapper.unmount()
  })

  it.each(['0', '-1', '60/0', '1/2147483648', '1e2', '59.94Hz', '60000/1001/2'])('refuses invalid refresh rate %s before sending', async text => {
    const wrapper = mountPreferences()
    await wrapper.get('[data-display-rate]').setValue(text)
    await wrapper.get('button').trigger('click')
    expect(wrapper.emitted('save')).toBeUndefined()
    expect(wrapper.get('[role="alert"]').text()).toBe('display_preferences.invalid')
    wrapper.unmount()
  })

  it('requires paired dimensions and permits explicitly clearing both mode overrides', async () => {
    const wrapper = mountPreferences({ force_virtual: true, resolution: { width: 1920, height: 1080 }, refresh_rate: { numerator: 60000, denominator: 1001 } })
    await wrapper.get('[data-display-height]').setValue('')
    await wrapper.get('button').trigger('click')
    expect(wrapper.emitted('save')).toBeUndefined()
    await wrapper.get('[data-display-width]').setValue('')
    await wrapper.get('[data-display-rate]').setValue('')
    await wrapper.get('button').trigger('click')
    expect(wrapper.emitted('save')).toEqual([[{ force_virtual: true, resolution: null, refresh_rate: null }]])
    wrapper.unmount()
  })

  it('shows invalid saved state and follows authoritative readback after a failed edit', async () => {
    const preferences = { force_virtual: false, resolution: null, refresh_rate: null }
    const wrapper = mountPreferences(preferences, { valid: false })
    expect(wrapper.get('[role="alert"]').text()).toBe('display_preferences.invalid_saved')
    await wrapper.get('[data-display-force]').setValue(true)
    await wrapper.setProps({ preferences: { ...preferences }, valid: true })
    expect(wrapper.get('[data-display-force]').element.checked).toBe(false)
    expect(wrapper.find('[role="alert"]').exists()).toBe(false)
    await wrapper.setProps({ pending: true })
    expect(wrapper.get('fieldset').attributes('disabled')).toBeDefined()
    wrapper.unmount()
  })
})
