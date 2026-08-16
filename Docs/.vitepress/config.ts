import { defineConfig } from 'vitepress'

export default defineConfig({
  title: 'html2.link',
  description: 'URL 一键生成 Android APK',
  lang: 'zh-CN',
  base: '/',

  head: [
    ['link', { rel: 'icon', href: '/html2link-logo.png' }]
  ],

  themeConfig: {
    logo: '/html2link-logo.png',
    siteTitle: 'html2.link',

    nav: [
      { text: '指南', link: '/guide/introduction' },
      { text: '参考', link: '/reference/features' },
      {
        text: 'v1.0.0',
        items: [
          { text: '更新日志', link: '/changelog' }
        ]
      }
    ],

    sidebar: [
      {
        text: '开始',
        items: [
          { text: '介绍', link: '/guide/introduction' },
          { text: '快速开始', link: '/guide/quickstart' },
          { text: '部署流程', link: '/guide/deploy' }
        ]
      },
      {
        text: '参考',
        items: [
          { text: '功能特性', link: '/reference/features' },
          { text: '常见问题', link: '/reference/faq' }
        ]
      }
    ],

    socialLinks: [
      { icon: 'github', link: 'https://github.com/wangrunjie1/Pakr' }
    ],

    footer: {
      message: 'Released under the MIT License.',
      copyright: 'html2.link'
    },

    search: {
      provider: 'local'
    },

    editLink: {
      pattern: 'https://github.com/wangrunjie1/Pakr/edit/main/Docs/:path',
      text: '在 GitHub 上编辑此页'
    },

    lastUpdated: {
      text: '最后更新于'
    }
  }
})
