<template>
  <Navbar></Navbar>
  <div id="content" class="container">
    <h1 class="my-4 text-center">{{ $t('pin.pin_pairing') }}</h1>
    <form class="form d-flex flex-column align-items-center" id="form" @submit.prevent="registerDevice">
      <div class="card flex-column d-flex p-4 mb-4">
        <div class="input-group mt-2">
          <label for="pairing-input" class="visually-hidden">{{ $t('pin.select_pairing') }}</label>
          <span class="input-group-text">
            <user-round-search :size="18" class="icon"></user-round-search>
          </span>
          <select v-model="selectedPairingId" class="form-select" id="pairing-input" required>
            <option disabled value="">
              {{ pendingPairings.length ? $t('pin.select_pairing') : $t('pin.no_pending_pairings') }}
            </option>
            <option v-for="pairing in pendingPairings" :key="pairing.id" :value="pairing.id">
              {{ pairing.name || $t('pin.unknown_device') }} — {{ pairing.address || $t('pin.unknown_address') }}
            </option>
          </select>
          <button
            type="button"
            class="btn btn-outline-danger"
            :disabled="!selectedPairingId"
            :title="$t('pin.cancel_pairing')"
            @click="cancelSelectedPairing"
          >
            <x :size="18" class="icon"></x>
          </button>
        </div>
        <div class="input-group mt-2">
          <label for="pin-input" class="visually-hidden">{{ $t('navbar.pin') }}</label>
          <span class="input-group-text">
            <hash :size="18" class="icon"></hash>
          </span>
          <input v-model="pin" type="text" pattern="\d{4}" maxlength="4" inputmode="numeric" :placeholder="`${$t('navbar.pin')}`" id="pin-input" class="form-control" required />
        </div>
        <div class="input-group my-4">
          <label for="name-input" class="visually-hidden">{{ $t('pin.device_name') }}</label>
          <span class="input-group-text">
            <monitor :size="18" class="icon"></monitor>
          </span>
          <input v-model="name" type="text" :placeholder="`${$t('pin.device_name')}`" id="name-input" class="form-control" required />
        </div>
        <button type="submit" class="btn btn-primary">
          <forward :size="18" class="icon"></forward>
          {{ $t('pin.send') }}
        </button>
      </div>
      <div class="alert alert-warning">
        <b>{{ $t('_common.warning') }}</b> {{ $t('pin.warning_msg') }}
      </div>
      <div v-if="status" :class="`alert alert-${status.type}`" role="alert">{{ status.message }}</div>
    </form>

    <section class="card p-4 mb-4 mx-auto" aria-labelledby="artemis-pairing-title">
      <h2 id="artemis-pairing-title" class="h4">{{ $t('pin.artemis_title') }}</h2>
      <p>{{ $t('pin.artemis_address_help') }}</p>
      <div class="row g-3 mb-3">
        <div class="col-sm-8">
          <label for="artemis-host" class="form-label">{{ $t('pin.artemis_host') }}</label>
          <input id="artemis-host" v-model="artemisHost" class="form-control" type="text" autocomplete="off" autocapitalize="none" spellcheck="false" />
        </div>
        <div class="col-sm-4">
          <label for="artemis-port" class="form-label">{{ $t('pin.artemis_port') }}</label>
          <input id="artemis-port" v-model="artemisPortOverride" class="form-control" type="text" inputmode="numeric" autocomplete="off" :placeholder="artemisGamePort || undefined" />
        </div>
      </div>
      <div class="d-flex flex-wrap gap-2">
        <button type="button" class="btn btn-primary" :disabled="!!artemisOperation" @click="generateArtemisInvitation">
          {{ $t('pin.artemis_generate') }}
        </button>
        <button type="button" class="btn btn-outline-danger" :disabled="!artemisInvitation || !!artemisOperation" @click="withdrawArtemisInvitation">
          {{ $t('pin.artemis_withdraw') }}
        </button>
      </div>
      <div v-if="artemisStatus" :class="`alert alert-${artemisStatus.type} mt-3 mb-0`" role="status">
        {{ $t(artemisStatus.message) }}
      </div>
      <div v-if="artemisInvitation?.credentials" class="mt-3">
        <p>{{ $t('pin.artemis_instructions') }}</p>
        <p>{{ $t('pin.artemis_expires_in', {seconds: artemisSecondsLeft}) }}</p>
        <dl class="row">
          <dt class="col-sm-3">{{ $t('navbar.pin') }}</dt>
          <dd id="artemis-pin" class="col-sm-9"><code>{{ artemisInvitation.credentials.pin }}</code></dd>
          <dt class="col-sm-3">{{ $t('pin.artemis_passphrase') }}</dt>
          <dd id="artemis-passphrase" class="col-sm-9"><code>{{ artemisInvitation.credentials.passphrase }}</code></dd>
        </dl>
        <div v-if="artemisLink" class="d-flex flex-column align-items-start gap-2">
          <a id="artemis-link" class="btn btn-outline-primary" :href="artemisLink" referrerpolicy="no-referrer" @click="guardArtemisLink">
            {{ $t('pin.artemis_open_link') }}
          </a>
          <img
            v-if="artemisQrImage"
            :key="artemisQrRevision"
            ref="artemisQr"
            class="img-fluid"
            :src="artemisQrImage"
            :alt="$t('pin.artemis_qr_alt')"
            @error="handleArtemisQrError"
          />
          <p v-else class="alert alert-warning mb-0">{{ $t('pin.artemis_qr_failure') }}</p>
        </div>
        <p v-else class="alert alert-warning mb-0">{{ $t('pin.artemis_invalid_address') }}</p>
      </div>
    </section>
  </div>
</template>

<script>
  import Navbar from './Navbar.vue'
  import { apiFetch } from './fetch_utils'
  import {
    artemisPairingLink,
    artemisQrDataUrl,
    defaultArtemisHost,
    mutateArtemisInvitation,
    parseArtemisInvitation,
  } from './artemis_pairing'
  import {
    Forward,
    Hash,
    Monitor,
    UserRoundSearch,
    X,
  } from '@lucide/vue'

  export default {
    components: {
      Navbar,
      Forward,
      Hash,
      Monitor,
      UserRoundSearch,
      X,
    },
    inject: ['i18n'],
    data() {
      return {
        artemisHost: defaultArtemisHost(window.location.hostname),
        artemisPortOverride: '',
        artemisGamePort: null,
        artemisInvitation: null,
        artemisLink: '',
        artemisOperation: null,
        artemisQrImage: '',
        artemisQrRevision: 0,
        artemisSecondsLeft: 0,
        artemisStatus: null,
        artemisTimer: null,
        name: '',
        pendingPairings: [],
        pin: '',
        refreshTimer: null,
        selectedPairingId: '',
        status: null,
      };
    },
    watch: {
      artemisHost: 'updateArtemisDisplay',
      artemisPortOverride: 'updateArtemisDisplay',
    },
    mounted() {
      this.loadPendingPairings();
      this.refreshTimer = window.setInterval(() => this.loadPendingPairings(), 2000);
      document.addEventListener('visibilitychange', this.updateArtemisCountdown);
      window.addEventListener('pagehide', this.resetArtemis);
    },
    beforeUnmount() {
      window.clearInterval(this.refreshTimer);
      document.removeEventListener('visibilitychange', this.updateArtemisCountdown);
      window.removeEventListener('pagehide', this.resetArtemis);
      this.resetArtemis();
    },
    methods: {
      /**
       * Refresh the authenticated list of pairing requests awaiting approval.
       */
      async loadPendingPairings() {
        try {
          const response = await apiFetch('./api/pin', {method: 'GET'});
          if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
          }

          const body = await response.json();
          this.pendingPairings = body.pairings || [];
          if (!this.pendingPairings.some((pairing) => pairing.id === this.selectedPairingId)) {
            this.selectedPairingId = this.pendingPairings.length === 1 ? this.pendingPairings[0].id : '';
          }
        } catch (error) {
          console.error('Failed to load pending pairing requests', error);
          this.status = {type: 'danger', message: this.i18n.t('pin.load_failure')};
        }
      },

      /**
       * Apply the entered PIN only to the pairing request selected by the operator.
       */
      async registerDevice() {
        this.status = null;
        if (!this.selectedPairingId) {
          this.status = {type: 'danger', message: this.i18n.t('pin.select_pairing_required')};
          return;
        }

        const body = JSON.stringify({
          pairing_id: this.selectedPairingId,
          pin: this.pin,
          name: this.name,
        });
        const response = await apiFetch('./api/pin', {
          method: "POST",
          headers: {
            'Content-Type': 'application/json'
          },
          body,
        });
        const result = await response.json();
        if (result.status === true) {
          this.status = {type: 'success', message: this.i18n.t('pin.pair_success')};
          this.pin = '';
          this.name = '';
        } else {
          this.status = {type: 'danger', message: this.i18n.t('pin.pair_failure')};
        }
        await this.loadPendingPairings();
      },

      /**
       * Cancel the selected pending request without affecting other clients.
       */
      async cancelSelectedPairing() {
        if (!this.selectedPairingId) {
          return;
        }

        const response = await apiFetch('./api/pin', {
          method: 'DELETE',
          headers: {
            'Content-Type': 'application/json',
          },
          body: JSON.stringify({pairing_id: this.selectedPairingId}),
        });
        const result = await response.json();
        this.status = result.status === true
          ? {type: 'success', message: this.i18n.t('pin.cancel_success')}
          : {type: 'danger', message: this.i18n.t('pin.cancel_failure')};
        await this.loadPendingPairings();
      },

      async generateArtemisInvitation() {
        if (this.artemisOperation) {
          return;
        }
        const operation = this.beginArtemisOperation();
        try {
          const {response, startedAt} = await mutateArtemisInvitation('POST', {}, operation.signal);
          operation.signal.throwIfAborted();
          const invitation = response.ok ? parseArtemisInvitation(await response.json(), startedAt) : null;
          operation.signal.throwIfAborted();
          if (!invitation) {
            this.artemisStatus = {type: 'danger', message: 'pin.artemis_generate_failure'};
            return;
          }
          this.artemisGamePort = invitation.port;
          this.artemisInvitation = invitation;
          this.updateArtemisDisplay();
        } catch {
          if (!operation.signal.aborted) {
            this.artemisStatus = {type: 'danger', message: 'pin.artemis_generate_failure'};
          }
        } finally {
          if (this.artemisOperation === operation) {
            this.artemisOperation = null;
          }
        }
      },

      async withdrawArtemisInvitation() {
        this.updateArtemisCountdown();
        if (!this.artemisInvitation || this.artemisOperation) {
          return;
        }
        const {generation} = this.artemisInvitation;
        const operation = this.beginArtemisOperation();
        try {
          const {response} = await mutateArtemisInvitation('DELETE', {generation}, operation.signal);
          operation.signal.throwIfAborted();
          const withdrawn = response.ok && (await response.json())?.status === true;
          operation.signal.throwIfAborted();
          if (withdrawn || response.status === 409) {
            this.clearArtemisInvitation();
            this.artemisStatus = withdrawn
              ? {type: 'success', message: 'pin.artemis_withdrawn'}
              : {type: 'warning', message: 'pin.artemis_unavailable'};
          } else {
            this.artemisStatus = {type: 'danger', message: 'pin.artemis_withdraw_failure'};
          }
        } catch {
          if (!operation.signal.aborted) {
            this.artemisStatus = {type: 'danger', message: 'pin.artemis_withdraw_failure'};
          }
        } finally {
          if (this.artemisOperation === operation) {
            this.artemisOperation = null;
          }
        }
      },

      beginArtemisOperation() {
        // Keep the generation available for an explicit withdrawal retry.
        this.clearArtemisCredentials();
        this.artemisStatus = null;
        this.artemisOperation = new AbortController();
        return this.artemisOperation;
      },

      updateArtemisDisplay() {
        this.updateArtemisCountdown();
        const credentials = this.artemisInvitation?.credentials;
        this.artemisQrImage = '';
        this.artemisLink = credentials ? artemisPairingLink(this.artemisHost, this.artemisPortOverride, this.artemisGamePort, credentials) : '';
        if (!this.artemisLink) {
          return;
        }
        try {
          this.artemisQrImage = artemisQrDataUrl(this.artemisLink);
          this.artemisQrRevision += 1;
        } catch {
          // The typed credentials and link remain available if encoding fails.
        }
      },

      updateArtemisCountdown() {
        window.clearTimeout(this.artemisTimer);
        if (!this.artemisInvitation) {
          return;
        }
        const {credentials, steadyDeadline, wallDeadline} = this.artemisInvitation;
        // Wall time accounts for sleep on platforms where the steady clock pauses.
        const remaining = Math.min(steadyDeadline - performance.now(), wallDeadline - Date.now());
        if (remaining <= 0) {
          this.clearArtemisInvitation();
          if (credentials) {
            this.artemisStatus = {type: 'warning', message: 'pin.artemis_expired'};
          }
          return;
        }
        this.artemisSecondsLeft = Math.ceil(remaining / 1000);
        this.artemisTimer = window.setTimeout(this.updateArtemisCountdown, Math.ceil(remaining % 1000 || 1000));
      },

      clearArtemisInvitation() {
        window.clearTimeout(this.artemisTimer);
        this.artemisTimer = null;
        this.clearArtemisCredentials();
        this.artemisInvitation = null;
        this.artemisSecondsLeft = 0;
      },

      clearArtemisCredentials() {
        if (this.artemisInvitation) {
          this.artemisInvitation.credentials = null;
        }
        this.artemisLink = '';
        this.artemisQrImage = '';
      },

      resetArtemis() {
        this.artemisOperation?.abort();
        this.artemisOperation = null;
        this.clearArtemisInvitation();
        this.artemisStatus = null;
      },

      guardArtemisLink(event) {
        this.updateArtemisCountdown();
        if (event.currentTarget.getAttribute('href') !== this.artemisLink) {
          event.preventDefault();
        }
      },

      handleArtemisQrError(event) {
        if (event.currentTarget === this.$refs.artemisQr && event.currentTarget.getAttribute('src') === this.artemisQrImage) {
          this.artemisQrImage = '';
        }
      },
    }
  }
</script>
