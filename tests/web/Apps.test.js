import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { flushPromises, mount } from '@vue/test-utils'

vi.mock('../../src_assets/common/assets/web/Navbar.vue', () => ({
  default: { template: '<div />' },
}))
vi.mock('../../src_assets/common/assets/web/fetch_utils', () => ({
  apiFetch: vi.fn(() => Promise.resolve({ status: 200 })),
}))

import { apiFetch } from '../../src_assets/common/assets/web/fetch_utils'
import Apps from '../../src_assets/common/assets/web/Apps.vue'

beforeEach(() => vi.clearAllMocks())
afterEach(() => vi.unstubAllGlobals())

describe('application display preferences', () => {
  it('edits force-virtual without changing stable identities or unknown typed metadata', () => {
    const app = { name: 'Game', id: '17', uuid: 'aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee', foreign: { flags: [false, null] } }
    const context = { apps: [app], platform: 'windows', openEditModal: vi.fn() }
    Apps.methods.editApp.call(context, 0)
    expect(context.editForm['force-virtual-display']).toBe(false)
    context.editForm['force-virtual-display'] = true
    expect(context.editForm.uuid).toBe(app.uuid)
    expect(context.editForm.id).toBe(app.id)
    expect(context.editForm.foreign).toEqual(app.foreign)
    expect(app['force-virtual-display']).toBeUndefined()
  })
})

/**
 * Build a minimal context object that satisfies
 * the `this` contract of fileBrowserConfirm().
 *
 * @param {string} path Selected file path.
 * @param {string[]|null} acceptedExtensions Allowed extensions or null.
 * @param {string} type Browser type (e.g. 'file', 'directory').
 * @returns {object} A stub context for `.call()`.
 */
function browserContext(path, acceptedExtensions, type = 'file') {
  return {
    fileBrowserSelectedPath: path,
    fileBrowserTypedPath: '',
    fileBrowserAcceptedExtensions: acceptedExtensions,
    fileBrowserType: type,
    fileBrowserError: '',
    fileBrowserCallback: vi.fn(),
    fileBrowserClose: vi.fn(),
    $t: (key, _params) => key,
  }
}

/**
 * Build a minimal context object that satisfies
 * the `this` contract of save().
 *
 * @param {string} imagePath The image-path value in the edit form.
 * @returns {object} A stub context for `.call()`.
 */
function saveContext(imagePath) {
  const modalBody = { scrollTop: 100 }
  return {
    editForm: { 'image-path': imagePath },
    editFormError: '',
    catalogRevision: 'revision-A',
    $refs: { editModal: { querySelector: () => modalBody } },
    $t: (key, _params) => key,
    _modalBody: modalBody,
  }
}

describe('fileBrowserConfirm – extension validation', () => {
  it('rejects a non-PNG file when acceptedExtensions is [".png"]', () => {
    const ctx = browserContext('/covers/cover.jpg', ['.png'])
    const onSelect = ctx.fileBrowserCallback

    Apps.methods.fileBrowserConfirm.call(ctx)

    expect(ctx.fileBrowserError).toBe('file_browser.error_invalid_extension')
    expect(onSelect).not.toHaveBeenCalled()
    expect(ctx.fileBrowserClose).not.toHaveBeenCalled()
  })

  it('accepts a PNG with mixed-case extension', () => {
    const ctx = browserContext('/covers/cover.PNG', ['.png'])
    const onSelect = ctx.fileBrowserCallback

    Apps.methods.fileBrowserConfirm.call(ctx)

    expect(ctx.fileBrowserError).toBe('')
    expect(onSelect).toHaveBeenCalledWith('/covers/cover.PNG')
    expect(ctx.fileBrowserClose).toHaveBeenCalledOnce()
  })

  it('allows any extension when acceptedExtensions is null', () => {
    const ctx = browserContext('/output/log.txt', null)
    const onSelect = ctx.fileBrowserCallback

    Apps.methods.fileBrowserConfirm.call(ctx)

    expect(ctx.fileBrowserError).toBe('')
    expect(onSelect).toHaveBeenCalledWith('/output/log.txt')
    expect(ctx.fileBrowserClose).toHaveBeenCalledOnce()
  })

  it('skips extension check for directory type even with acceptedExtensions', () => {
    const ctx = browserContext('/some/directory', ['.png'], 'directory')
    const onSelect = ctx.fileBrowserCallback

    Apps.methods.fileBrowserConfirm.call(ctx)

    expect(ctx.fileBrowserError).toBe('')
    expect(onSelect).toHaveBeenCalledWith('/some/directory')
    expect(ctx.fileBrowserClose).toHaveBeenCalledOnce()
  })

  it('does nothing when no path is selected', () => {
    const ctx = browserContext('', ['.png'])
    const onSelect = ctx.fileBrowserCallback

    Apps.methods.fileBrowserConfirm.call(ctx)

    expect(ctx.fileBrowserError).toBe('')
    expect(onSelect).not.toHaveBeenCalled()
    expect(ctx.fileBrowserClose).not.toHaveBeenCalled()
  })

  it('uses fileBrowserTypedPath as fallback when selectedPath is empty', () => {
    const ctx = browserContext('', ['.png'])
    ctx.fileBrowserTypedPath = '/covers/art.png'
    const onSelect = ctx.fileBrowserCallback

    Apps.methods.fileBrowserConfirm.call(ctx)

    expect(onSelect).toHaveBeenCalledWith('/covers/art.png')
    expect(ctx.fileBrowserClose).toHaveBeenCalledOnce()
  })
})

describe('save – cover image validation', () => {
  it('blocks a manually typed non-PNG image path', () => {
    const ctx = saveContext('/covers/cover.jpg')

    Apps.methods.save.call(ctx)

    expect(ctx.editFormError).toBe('file_browser.error_invalid_extension')
    expect(ctx._modalBody.scrollTop).toBe(0)
    expect(apiFetch).not.toHaveBeenCalled()
  })

  it('blocks a .bmp image path', () => {
    const ctx = saveContext('/covers/image.bmp')

    Apps.methods.save.call(ctx)

    expect(ctx.editFormError).toBe('file_browser.error_invalid_extension')
    expect(apiFetch).not.toHaveBeenCalled()
  })

  it('accepts a valid .PNG path (mixed case) and submits', () => {
    const ctx = saveContext('/covers/cover.PnG')

    Apps.methods.save.call(ctx)

    expect(ctx.editFormError).toBe('')
    expect(apiFetch).toHaveBeenCalledOnce()
    expect(apiFetch).toHaveBeenCalledWith(
      './api/apps',
      expect.objectContaining({ method: 'POST' })
    )
  })

  it('submits when the image path is empty', () => {
    const ctx = saveContext('')

    Apps.methods.save.call(ctx)

    expect(ctx.editFormError).toBe('')
    expect(apiFetch).toHaveBeenCalledOnce()
  })

  it('strips double quotes from the image path before validating', () => {
    const ctx = saveContext('""/covers/cover.png""')

    Apps.methods.save.call(ctx)

    expect(ctx.editForm['image-path']).toBe('/covers/cover.png')
    expect(ctx.editFormError).toBe('')
    expect(apiFetch).toHaveBeenCalledOnce()
  })
})

/**
 * @brief Create file-browser state with an existing selection for navigation tests.
 *
 * @param {string} type Browser selection type.
 * @return {object} Context for fileBrowserNavigate().
 */
function navigationContext(type = 'file') {
  return {
    fileBrowserType: type,
    fileBrowserLoading: false,
    fileBrowserError: 'Previous error',
    fileBrowserCurrentPath: '/old',
    fileBrowserParentPath: '/',
    fileBrowserEntries: [{ name: 'old.png', path: '/old/old.png', type: 'file' }],
    fileBrowserTypedPath: '/old/old.png',
    fileBrowserSelectedPath: '/old/old.png',
  }
}

describe('fileBrowserNavigate', () => {
  it.each(['file', 'directory'])('loads a listing and updates the %s selection', async type => {
    const ctx = navigationContext(type)
    const entries = [{ name: 'cover.png', path: '/covers/cover.png', type: 'file' }]
    const response = Promise.withResolvers()
    vi.stubGlobal('fetch', vi.fn(() => response.promise))

    const navigation = Apps.methods.fileBrowserNavigate.call(ctx, '/covers')
    expect(ctx.fileBrowserLoading).toBe(true)
    expect(ctx.fileBrowserError).toBe('')
    expect(fetch).toHaveBeenCalledExactlyOnceWith(`./api/browse?type=${type}&path=%2Fcovers`)

    response.resolve({ ok: true, json: async () => ({ path: '/covers', parent: '/', entries }) })
    await navigation

    expect(ctx.fileBrowserCurrentPath).toBe('/covers')
    expect(ctx.fileBrowserParentPath).toBe('/')
    expect(ctx.fileBrowserEntries).toEqual(entries)
    expect(ctx.fileBrowserTypedPath).toBe('/covers')
    expect(ctx.fileBrowserSelectedPath).toBe(type === 'directory' ? '/covers' : '')
    expect(ctx.fileBrowserError).toBe('')
    expect(ctx.fileBrowserLoading).toBe(false)
  })

  it.each(['file', 'directory'])('uses defaults for missing fields in a %s listing', async type => {
    const ctx = navigationContext(type)
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue({ ok: true, json: async () => ({}) }))

    await Apps.methods.fileBrowserNavigate.call(ctx, '')

    expect(fetch).toHaveBeenCalledExactlyOnceWith(`./api/browse?type=${type}`)
    expect(ctx.fileBrowserCurrentPath).toBe('')
    expect(ctx.fileBrowserParentPath).toBe('')
    expect(ctx.fileBrowserEntries).toEqual([])
    expect(ctx.fileBrowserTypedPath).toBe('')
    expect(ctx.fileBrowserSelectedPath).toBe('')
    expect(ctx.fileBrowserLoading).toBe(false)
  })

  it.each([
    [{ error: 'Permission denied' }, 'Permission denied'],
    [{}, 'Browse failed'],
  ])('displays HTTP errors while retaining the previous listing', async (body, message) => {
    const ctx = navigationContext()
    const previousEntries = ctx.fileBrowserEntries
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue({ ok: false, json: async () => body }))

    await Apps.methods.fileBrowserNavigate.call(ctx, '/restricted')

    expect(ctx.fileBrowserError).toBe(message)
    expect(ctx.fileBrowserLoading).toBe(false)
    expect(ctx.fileBrowserCurrentPath).toBe('/old')
    expect(ctx.fileBrowserEntries).toBe(previousEntries)
    expect(ctx.fileBrowserTypedPath).toBe('/old/old.png')
    expect(ctx.fileBrowserSelectedPath).toBe('/old/old.png')
  })

  it('displays network failures and clears the loading state', async () => {
    const ctx = navigationContext()
    vi.stubGlobal('fetch', vi.fn().mockRejectedValue(new Error('Network unavailable')))

    await Apps.methods.fileBrowserNavigate.call(ctx, '/covers')

    expect(ctx.fileBrowserError).toBe('Network unavailable')
    expect(ctx.fileBrowserLoading).toBe(false)
    expect(ctx.fileBrowserCurrentPath).toBe('/old')
  })

  it.each([true, false])('displays invalid JSON errors when response.ok is %s', async ok => {
    const ctx = navigationContext()
    const json = vi.fn().mockRejectedValue(new Error('Invalid JSON'))
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue({ ok, json }))

    await Apps.methods.fileBrowserNavigate.call(ctx, '/covers')

    expect(ctx.fileBrowserError).toBe('Invalid JSON')
    expect(ctx.fileBrowserLoading).toBe(false)
    expect(ctx.fileBrowserCurrentPath).toBe('/old')
  })
})


describe('application catalog revisions', () => {
  it('sends the fetched revision while preserving hidden identity', async () => {
    const ctx = saveContext('');
    ctx.editForm.uuid = '11111111-1111-1111-1111-111111111111';
    ctx.editForm.id = '42';
    vi.stubGlobal('document', { location: { reload: vi.fn() } });
    await Apps.methods.save.call(ctx);
    expect(apiFetch).toHaveBeenCalledWith('./api/apps', expect.objectContaining({
      headers: expect.objectContaining({ 'If-Match': 'revision-A' }),
      body: JSON.stringify(ctx.editForm),
    }));
    expect(document.location.reload).toHaveBeenCalledOnce();
  });

  it.each([409, 428, 500])('retains the form and displays a %s failure', async status => {
    const ctx = saveContext('');
    const form = ctx.editForm;
    apiFetch.mockResolvedValueOnce({ status, json: async () => ({ error: 'Reload the catalog before retrying' }) });
    vi.stubGlobal('document', { location: { reload: vi.fn() } });
    await Apps.methods.save.call(ctx);
    expect(ctx.editForm).toBe(form);
    expect(ctx.editFormError).toBe('Reload the catalog before retrying');
    expect(document.location.reload).not.toHaveBeenCalled();
  });

  it('deletes by UUID with a revision and retains the target after a conflict', async () => {
    const uuid = '11111111-1111-1111-1111-111111111111';
    const ctx = { deleteTarget: { uuid }, catalogRevision: 'revision-A', deleteError: '', $t: key => key };
    apiFetch.mockResolvedValueOnce({ status: 409, json: async () => ({ error: 'Catalog changed' }) });
    vi.stubGlobal('document', { location: { reload: vi.fn() } });
    await Apps.methods.confirmDelete.call(ctx);
    expect(apiFetch).toHaveBeenCalledWith('./api/apps/' + uuid, expect.objectContaining({
      method: 'DELETE', headers: expect.objectContaining({ 'If-Match': 'revision-A' }),
    }));
    expect(ctx.deleteError).toBe('Catalog changed');
    expect(ctx.deleteTarget.uuid).toBe(uuid);
    expect(document.location.reload).not.toHaveBeenCalled();
  });

  it('loads committed identities and revision together', async () => {
    const apps = [{ name: 'A', id: '42', uuid: '11111111-1111-1111-1111-111111111111' }];
    const ctx = { apps: [], catalogRevision: '', catalogError: '', $t: key => key };
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue({ ok: true, json: async () => ({ apps, revision: 'revision-B' }) }));
    await Apps.methods.loadApps.call(ctx);
    expect(ctx.apps).toBe(apps);
    expect(ctx.catalogRevision).toBe('revision-B');
  });
});

describe('Artemis application shortcuts', () => {
  const hostUuid = 'A1111111-1111-4111-8111-111111111111'
  const appUuid = 'b2222222-2222-4222-8222-222222222222'
  const otherAppUuid = 'c3333333-3333-4333-8333-333333333333'
  const open = 'a[href^="art://"]'
  const downloadLabel = 'apps.artemis_download'
  let page
  let fetchCatalog

  function catalogResponse(overrides = {}) {
    return {
      ok: true,
      json: async () => ({
        apps: [{ uuid: appUuid, name: 'R&D + 50%' }],
        revision: 'revision-A',
        host_uuid: hostUuid,
        host_name: 'Room A',
        ...overrides,
      }),
    }
  }

  async function mountApps(overrides) {
    fetchCatalog = vi.fn().mockResolvedValue(catalogResponse(overrides))
    vi.stubGlobal('fetch', vi.fn(url => {
      if (url === './api/apps') return fetchCatalog()
      if (url === './api/config') {
        return Promise.resolve({ json: async () => ({ platform: 'windows', version: '1.0.0' }) })
      }
      if (url === 'https://api.github.com/repos/LizardByte/Sunshine/releases/latest') {
        return Promise.resolve({ json: async () => ({ tag_name: 'v1.0.0' }) })
      }
      throw new Error('Unexpected request ' + url)
    }))
    page = mount(Apps, { global: { mocks: { $t: key => key }, stubs: { 'i18n-t': true } } })
    await flushPromises()
  }

  function downloadButton(card = page) {
    return card.findAll('button').find(button => button.text() === downloadLabel)
  }

  afterEach(() => {
    page?.unmount()
    page = undefined
    vi.restoreAllMocks()
  })

  it('opens and downloads matching selectors with safely encoded labels and no application operation', async () => {
    const hostName = ' Room A + %26 & # "東京"\r\n\u2028[app_uuid] ignored \uD800 '
    const appName = ' ../../CON\\R&D + 50% 🎮\u0000\u0085\u2029[host_uuid] "app" \uDC00 '
    await mountApps({
      host_name: hostName,
      apps: [{ id: '42', uuid: appUuid, name: appName }],
      address: 'should-not-be-used.invalid:1234',
    })
    const createObjectURL = vi.fn().mockReturnValue('blob:artemis-shortcut')
    const revokeObjectURL = vi.fn()
    vi.stubGlobal('URL', Object.assign(class extends URL {}, { createObjectURL, revokeObjectURL }))
    const expectedFields = {
      host_uuid: hostUuid,
      host_name: 'Room A + %26 & # "東京" [app_uuid] ignored �',
      app_uuid: appUuid,
      app_name: '../../CON\\R&D + 50% 🎮 [host_uuid] "app" �',
    }

    const link = page.get(open)
    const linkClick = vi.fn(event => {
      expect(event.defaultPrevented).toBe(false)
      event.preventDefault()
    })
    link.element.addEventListener('click', linkClick)
    expect(link.text()).toBe('apps.artemis_open')
    expect(linkClick).not.toHaveBeenCalled()
    expect(createObjectURL).not.toHaveBeenCalled()
    await link.trigger('click')
    expect(linkClick).toHaveBeenCalledOnce()

    const href = link.attributes('href')
    const uri = new URL(href)
    expect(href).toMatch(/^art:\/\/launch\?host_uuid=/)
    expect(uri.host).toBe('launch')
    expect(uri.pathname + uri.username + uri.password + uri.hash).toBe('')
    expect([...uri.searchParams.entries()]).toEqual(Object.entries(expectedFields))
    expect(href).toContain('Room%20A%20%2B%20%2526%20%26%20%23')

    const dispatch = vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(function () {
      expect(this.isConnected).toBe(true)
      expect(this.getAttribute('href')).toBe('blob:artemis-shortcut')
      expect(this.download).toBe('Miranda-' + appUuid + '.art')
      expect(revokeObjectURL).not.toHaveBeenCalled()
    })
    await downloadButton().trigger('click')
    expect(createObjectURL).toHaveBeenCalledOnce()
    expect(dispatch).toHaveBeenCalledOnce()
    expect(document.querySelector('a[download]')).toBeNull()
    await vi.waitFor(() => expect(revokeObjectURL).toHaveBeenCalledExactlyOnceWith('blob:artemis-shortcut'))

    const [[blob]] = createObjectURL.mock.calls
    expect(blob.type).toBe('text/plain;charset=utf-8')
    const bytes = await new Promise((resolve, reject) => {
      const reader = new FileReader()
      reader.onload = () => resolve(new Uint8Array(reader.result))
      reader.onerror = () => reject(reader.error)
      reader.readAsArrayBuffer(blob)
    })
    const expectedText = `[host_uuid] ${hostUuid}\n[host_name] ${expectedFields.host_name}\n[app_uuid] ${appUuid}\n[app_name] ${expectedFields.app_name}\n`
    expect([...bytes]).toEqual([...new TextEncoder().encode(expectedText)])
    const records = new TextDecoder().decode(bytes).split('\n').map(line => line.trim())
      .filter(line => line && !line.startsWith('#'))
      .map(line => {
        const space = line.indexOf(' ')
        return [line.slice(0, space).slice(1, -1), line.slice(space + 1).trim()]
      })
    expect(records).toEqual(Object.entries(expectedFields))

    expect(page.vm.apps[0].name).toBe(appName)
    expect(fetchCatalog).toHaveBeenCalledOnce()
    expect(fetch).toHaveBeenCalledTimes(3)
    expect(apiFetch).not.toHaveBeenCalled()
  })

  it('targets the committed row through duplicate names, sorting, filtering and an unsaved rename', async () => {
    const thirdAppUuid = 'd4444444-4444-4444-8444-444444444444'
    await mountApps({
      apps: [
        { uuid: appUuid, name: 'Room A' },
        { uuid: otherAppUuid, name: 'Room A' },
        { uuid: thirdAppUuid, name: 'A Room' },
      ],
    })
    const visibleUuids = () => page.findAll(open).map(link =>
      new URL(link.attributes('href')).searchParams.get('app_uuid')
    )
    expect(visibleUuids()).toEqual([appUuid, otherAppUuid, thirdAppUuid])

    await page.get('.apps-toolbar button[aria-pressed]').trigger('click')
    expect(visibleUuids()).toEqual([thirdAppUuid, appUuid, otherAppUuid])

    const edit = vi.spyOn(page.vm, 'openEditModal').mockImplementation(() => {})
    const firstCard = page.get('.app-card')
    await firstCard.findAll('button').find(button => button.text() === 'apps.edit').trigger('click')
    expect(edit).toHaveBeenCalledOnce()
    expect(page.vm.editForm.uuid).toBe(thirdAppUuid)
    page.vm.editForm.name = 'Unsaved app name'
    page.vm.editForm.uuid = appUuid
    await page.vm.$nextTick()
    const thirdUri = new URL(page.get(open).attributes('href'))
    expect(thirdUri.searchParams.get('app_uuid')).toBe(thirdAppUuid)
    expect(thirdUri.searchParams.get('app_name')).toBe('A Room')

    await page.get('#app-search').setValue('Room A')
    expect(visibleUuids()).toEqual([appUuid, otherAppUuid])
    page.vm.newApp()
    page.vm.editForm.uuid = thirdAppUuid
    await page.vm.$nextTick()
    expect(visibleUuids()).toEqual([appUuid, otherAppUuid])

    const createObjectURL = vi.fn().mockReturnValue('blob:filtered-shortcut')
    const revokeObjectURL = vi.fn()
    vi.stubGlobal('URL', Object.assign(class extends URL {}, { createObjectURL, revokeObjectURL }))
    const dispatch = vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(function () {
      expect(this.download).toBe('Miranda-' + otherAppUuid + '.art')
    })
    await downloadButton(page.findAll('.app-card')[1]).trigger('click')
    expect(dispatch).toHaveBeenCalledOnce()
    await vi.waitFor(() => expect(revokeObjectURL).toHaveBeenCalledExactlyOnceWith('blob:filtered-shortcut'))
    expect(apiFetch).not.toHaveBeenCalled()
  })

  it('omits empty optional labels and refuses missing or malformed app IDs', async () => {
    await mountApps({
      host_name: null,
      apps: [
        { uuid: appUuid, name: ' \u0000 ' },
        { id: '42', name: 'Room A' },
        { uuid: otherAppUuid + '\n', name: 'Room A' },
      ],
    })
    expect(page.findAll(open)).toHaveLength(1)
    expect(page.get(open).attributes('href')).toBe(`art://launch?host_uuid=${hostUuid}&app_uuid=${appUuid}`)
    for (const card of page.findAll('.app-card').slice(1)) {
      expect(card.text()).toContain('apps.artemis_app_unavailable')
      expect(downloadButton(card)).toBeUndefined()
    }
  })

  it.each([undefined, null, hostUuid + '\n', 'wrong-shape'])('keeps app management available without a stable host ID (%s)', async value => {
    await mountApps({ host_uuid: value })
    expect(page.text()).toContain('apps.artemis_host_unavailable')
    expect(page.find(open).exists()).toBe(false)
    expect(downloadButton()).toBeUndefined()
    expect(page.get('.app-card').text()).toContain('apps.edit')
  })

  it('invalidates existing shortcuts during reload and keeps them unavailable on failure', async () => {
    await mountApps()
    expect(page.find(open).exists()).toBe(true)

    const response = Promise.withResolvers()
    fetchCatalog.mockReturnValueOnce(response.promise)
    const reload = page.vm.loadApps()
    await page.vm.$nextTick()
    expect(page.find(open).exists()).toBe(false)
    expect(downloadButton()).toBeUndefined()

    response.resolve({ ok: false, json: async () => ({ error: 'Catalog failed', host_uuid: hostUuid }) })
    await reload
    expect(page.text()).toContain('Catalog failed')
    expect(page.find(open).exists()).toBe(false)
    expect(page.vm.catalogRevision).toBe('revision-A')

    fetchCatalog.mockResolvedValueOnce(catalogResponse({ host_uuid: null }))
    await page.vm.loadApps()
    expect(page.text()).toContain('apps.artemis_host_unavailable')
    expect(page.find(open).exists()).toBe(false)

    fetchCatalog.mockResolvedValueOnce(catalogResponse({ apps: [{ uuid: appUuid, name: 'Room A' }] }))
    await page.vm.loadApps()
    const params = new URL(page.get(open).attributes('href')).searchParams
    expect(params.get('app_uuid')).toBe(appUuid)
    expect(params.get('app_name')).toBe('Room A')
  })

  it('ignores a stale successful response after a later reload has invalidated shortcuts', async () => {
    await mountApps()
    const earlierResponse = Promise.withResolvers()
    fetchCatalog.mockReturnValueOnce(earlierResponse.promise)
    const earlierReload = page.vm.loadApps()
    fetchCatalog.mockResolvedValueOnce(catalogResponse({ host_uuid: null, revision: 'revision-B' }))
    await page.vm.loadApps()

    earlierResponse.resolve(catalogResponse())
    await earlierReload
    expect(page.find(open).exists()).toBe(false)
    expect(page.vm.catalogRevision).toBe('revision-B')
  })
})
