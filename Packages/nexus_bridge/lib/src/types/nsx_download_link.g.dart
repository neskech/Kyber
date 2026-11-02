// GENERATED CODE - DO NOT MODIFY BY HAND

part of 'nsx_download_link.dart';

// **************************************************************************
// JsonSerializableGenerator
// **************************************************************************

NsxDownloadLink _$NsxDownloadLinkFromJson(Map<String, dynamic> json) =>
    NsxDownloadLink(
      name: json['name'] as String,
      shortName: json['short_name'] as String,
      uri: json['URI'] as String,
    );

Map<String, dynamic> _$NsxDownloadLinkToJson(NsxDownloadLink instance) =>
    <String, dynamic>{
      'name': instance.name,
      'short_name': instance.shortName,
      'URI': instance.uri,
    };
