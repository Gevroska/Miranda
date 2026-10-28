<script setup>
import { ref, watch } from 'vue'

const props = defineProps({
  preferences: { type: Object, required: true },
  valid: { type: Boolean, default: true },
  pending: { type: Boolean, default: false },
  deviceId: { type: String, required: true },
})
const emit = defineEmits(['save'])
const forceVirtual = ref(false)
const width = ref('')
const height = ref('')
const refreshRate = ref('')
const error = ref(false)

watch(() => props.preferences, preferences => {
  forceVirtual.value = preferences.force_virtual ?? false
  width.value = preferences.resolution?.width?.toString() ?? ''
  height.value = preferences.resolution?.height?.toString() ?? ''
  const rate = preferences.refresh_rate
  refreshRate.value = rate ? (rate.denominator === 1 ? `${rate.numerator}` : `${rate.numerator}/${rate.denominator}`) : ''
  error.value = false
}, { immediate: true })

/** Parse exact decimal or fractional hertz without floating-point rounding. */
function parseRate(text) {
  if (!/^\d+(?:\/\d+|\.\d+)?$/.test(text) || text.length > 64) throw new Error('Invalid refresh rate')
  let [numerator, denominator] = text.includes('/') ? text.split('/') : [text.replace('.', ''), '1']
  if (text.includes('.')) denominator = `1${'0'.repeat(text.split('.')[1].length)}`
  let a = BigInt(numerator)
  let b = BigInt(denominator)
  if (a <= 0n || b <= 0n) throw new Error('Invalid refresh rate')
  const originalA = a
  const originalB = b
  while (b) [a, b] = [b, a % b]
  numerator = originalA / a
  denominator = originalB / a
  if (numerator > 2147483647n || denominator > 2147483647n) throw new Error('Refresh rate exceeds source range')
  return { numerator: Number(numerator), denominator: Number(denominator) }
}

/** Submit a complete known preference set; empty mode fields remove only the overrides. */
function save() {
  try {
    const w = `${width.value}`.trim()
    const h = `${height.value}`.trim()
    let resolution = null
    if (w || h) {
      if (!/^\d+$/.test(w) || !/^\d+$/.test(h) || Number(w) <= 0 || Number(h) <= 0 || Number(w) > 2147483647 || Number(h) > 2147483647) throw new Error('Invalid resolution')
      resolution = { width: Number(w), height: Number(h) }
    }
    const rate = `${refreshRate.value}`.trim()
    error.value = false
    emit('save', { force_virtual: forceVirtual.value, resolution, refresh_rate: rate ? parseRate(rate) : null })
  } catch {
    error.value = true
  }
}
</script>

<template>
  <fieldset class="mt-3" :disabled="pending">
    <legend class="fs-6">{{ $t('display_preferences.title') }}</legend>
    <p class="form-text">{{ $t('display_preferences.availability') }}</p>
    <p v-if="!valid" class="text-warning" role="alert">{{ $t('display_preferences.invalid_saved') }}</p>
    <label class="form-check">
      <input type="checkbox" class="form-check-input" v-model="forceVirtual" data-display-force>
      <span class="form-check-label">{{ $t('config.force_virtual_display') }}</span>
    </label>
    <div class="row g-2 mt-1">
      <div class="col-sm-4">
        <label :for="`display-width-${deviceId}`" class="form-label">{{ $t('display_preferences.width') }}</label>
        <input :id="`display-width-${deviceId}`" class="form-control" type="number" min="1" max="2147483647" step="1" v-model="width" data-display-width>
      </div>
      <div class="col-sm-4">
        <label :for="`display-height-${deviceId}`" class="form-label">{{ $t('display_preferences.height') }}</label>
        <input :id="`display-height-${deviceId}`" class="form-control" type="number" min="1" max="2147483647" step="1" v-model="height" data-display-height>
      </div>
      <div class="col-sm-4">
        <label :for="`display-rate-${deviceId}`" class="form-label">{{ $t('display_preferences.refresh_rate') }}</label>
        <input :id="`display-rate-${deviceId}`" class="form-control" type="text" placeholder="60000/1001" v-model="refreshRate" data-display-rate>
      </div>
    </div>
    <p class="form-text">{{ $t('display_preferences.mode_help') }}</p>
    <p v-if="error" class="text-danger" role="alert">{{ $t('display_preferences.invalid') }}</p>
    <button type="button" class="btn btn-primary btn-sm" @click="save">{{ $t('display_preferences.save') }}</button>
  </fieldset>
</template>
